#undef NDEBUG  // asserts must stay live regardless of build type
#include "core/conversation.h"
#include "core/message.h"
#include "core/sentinel_scanner.h"
#include "harness/harness.h"
#include "model/replay_client.h"
#include "model/scripted_client.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// Minimal test registry

namespace {

struct TestCase {
    const char* name;
    void (*fn)();
};

constexpr std::size_t kMaxTests = 64;
TestCase g_tests[kMaxTests];
std::size_t g_test_count = 0;

struct Registrar {
    Registrar(const char* name, void (*fn)()) {
        assert(g_test_count < kMaxTests && "raise kMaxTests");
        g_tests[g_test_count++] = {name, fn};
    }
};

}  // namespace

#define TEST(name)                                        \
    static void name();                                   \
    static const Registrar name##_registrar(#name, name); \
    static void name()

// Shared helpers

namespace {

const std::string kSentinel = "<|end_conversation|>";

// Scripted user input. Once every line is consumed, the next read_line()
// reports EOF — exactly like Ctrl-D on the real terminal.
class FakeInput : public InputSource {
public:
    explicit FakeInput(std::deque<std::string> lines) : lines_(std::move(lines)) {}

    std::string read_line() override {
        if (lines_.empty()) {
            eof_ = true;
            return "";
        }
        std::string line = std::move(lines_.front());
        lines_.pop_front();
        return line;
    }
    bool is_eof() const override { return eof_; }
    std::size_t remaining() const { return lines_.size(); }

private:
    std::deque<std::string> lines_;
    bool eof_ = false;
};

// Captures everything the harness would have printed to the terminal.
class CaptureOutput : public OutputSink {
public:
    void write(std::string_view text) override { text_ += text; }
    const std::string& text() const { return text_; }

private:
    std::string text_;
};

std::filesystem::path test_dir() {
    static const std::filesystem::path dir = [] {
        auto d = std::filesystem::temp_directory_path() / "miniharness_p2_tests";
        std::filesystem::create_directories(d);
        return d;
    }();
    return dir;
}

std::string write_file(const std::string& name, const std::string& contents) {
    const std::string path = (test_dir() / name).string();
    std::ofstream out(path);
    assert(out.is_open());
    out << contents;
    return path;
}

std::string read_file(const std::string& path) {
    std::ifstream in(path);
    assert(in.is_open());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

const char* role_name(Role role) {
    switch (role) {
        case Role::System: return "system";
        case Role::User: return "user";
        case Role::Assistant: return "assistant";
    }
    return "assistant";
}

// Same Appendix A format as save_transcript() in the provided main.cpp
// (which lives in an anonymous namespace there, so it can't be linked).
void save_transcript(const Conversation& conv, const std::string& path) {
    std::ofstream file(path);
    assert(file.is_open());
    bool first = true;
    for (const Message& m : conv) {
        if (!first) file << "---\n";
        first = false;
        file << "role: " << role_name(m.role()) << "\n";
        file << m.content() << "\n";
    }
}

bool same_messages(const Conversation& a, const Conversation& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (a.at(i).role() != b.at(i).role()) return false;
        if (a.at(i).content() != b.at(i).content()) return false;
    }
    return true;
}

Conversation make_conversation(std::size_t n) {
    Conversation c;
    for (std::size_t i = 0; i < n; ++i) {
        c.append(Message(i % 2 == 0 ? Role::User : Role::Assistant,
                         "message number " + std::to_string(i)));
    }
    return c;
}

// Feeds `text` to a fresh scanner in pieces of `piece` characters, then
// flushes. Returns the concatenated safe text; sets `found`.
std::string scan_in_pieces(const std::string& text, std::size_t piece, bool& found) {
    SentinelScanner scanner(kSentinel);
    std::string safe;
    found = false;
    for (std::size_t i = 0; i < text.size(); i += piece) {
        auto out = scanner.feed(std::string_view(text).substr(i, piece));
        safe += out.safe_text;
        found = found || out.sentinel_found;
        assert(scanner.pending_size() <= kSentinel.size() - 1);
    }
    auto out = scanner.flush();
    safe += out.safe_text;
    found = found || out.sentinel_found;
    return safe;
}

}  // namespace

// Conversation

// Spec item 1: empty conversations never touch memory out of bounds.
TEST(EmptyConversationBounds) {
    const Conversation conv;
    assert(conv.size() == 0);
    assert(conv.capacity() == 0);               // no allocation yet
    assert(conv.begin() == conv.end());
    assert(conv.begin() == nullptr);

    int iterations = 0;
    for (const Message& m : conv) { (void)m; ++iterations; }
    assert(iterations == 0);

    // Chosen out-of-range behaviour: throw std::out_of_range.
    bool threw = false;
    try { (void)conv.at(0); } catch (const std::out_of_range&) { threw = true; }
    assert(threw && "at(0) on an empty conversation must throw");

    threw = false;
    try { (void)conv.at(static_cast<std::size_t>(-1)); } catch (const std::out_of_range&) { threw = true; }
    assert(threw);

    // Non-empty: the last valid index works, one past it throws.
    Conversation one;
    one.append(Message(Role::User, "hi"));
    assert(one.at(0).content() == "hi");
    threw = false;
    try { (void)one.at(1); } catch (const std::out_of_range&) { threw = true; }
    assert(threw && "at(size()) must throw");
}

// Spec item 2: a System message is always first and never evicted.
TEST(SystemMessagePinnedFirst) {
    // Case A: system appended first stays at index 0 through many reallocations.
    Conversation conv;
    conv.append(Message(Role::System, "Be concise."));
    for (int i = 0; i < 200; ++i) {
        conv.append(Message(i % 2 ? Role::Assistant : Role::User, std::to_string(i)));
        assert(conv.at(0).role() == Role::System);
        assert(conv.at(0).content() == "Be concise.");
    }
    assert(conv.size() == 201);
    assert(conv.begin()->role() == Role::System);

    // Case B: system appended late is pinned to the front; the relative order
    // of the other messages is preserved.
    Conversation late;
    late.append(Message(Role::User, "u0"));
    late.append(Message(Role::Assistant, "a0"));
    late.append(Message(Role::User, "u1"));
    late.append(Message(Role::System, "sys"));
    assert(late.size() == 4);
    assert(late.at(0).role() == Role::System && late.at(0).content() == "sys");
    assert(late.at(1).role() == Role::User && late.at(1).content() == "u0");
    assert(late.at(2).role() == Role::Assistant && late.at(2).content() == "a0");
    assert(late.at(3).role() == Role::User && late.at(3).content() == "u1");

    // Case C: a second System message is rejected and the existing one is
    // untouched (never evicted / replaced).
    bool threw = false;
    try { late.append(Message(Role::System, "evil override")); }
    catch (const std::logic_error&) { threw = true; }
    assert(threw);
    assert(late.size() == 4);
    assert(late.at(0).content() == "sys");

    // Case D: the pin survives copy and move.
    Conversation copy = late;
    assert(copy.at(0).role() == Role::System);
    Conversation moved = std::move(copy);
    assert(moved.at(0).role() == Role::System && moved.at(0).content() == "sys");

    // Case E: under the provided Harness, cfg.system_message is at index 0.
    const std::string script = write_file("pin.script",
        "role: system\nBe concise.\n---\nrole: assistant\nOk.\n---\n"
        "role: assistant\nBye.<|end_conversation|>\n");
    auto model = std::make_unique<ScriptedModelClient>(script);
    HarnessConfig cfg;
    cfg.system_message = model->system_message();
    Harness harness(std::move(model), cfg);
    FakeInput in({"one", "two"});
    CaptureOutput out;
    harness.run(in, out);
    const Conversation& hc = harness.conversation();
    assert(hc.size() == 5);
    assert(hc.at(0).role() == Role::System && hc.at(0).content() == "Be concise.");
    for (std::size_t i = 1; i < hc.size(); ++i) assert(hc.at(i).role() != Role::System);
}

// Spec item 3: copy ctor / copy assignment allocate a distinct buffer.
TEST(RuleOfFiveCopyIsDeep) {
    Conversation original = make_conversation(5);

    // Copy constructor.
    Conversation copy(original);
    assert(copy.size() == original.size());
    assert(copy.begin() != original.begin() && "copy must own its own buffer");
    assert(same_messages(copy, original));
    for (std::size_t i = 0; i < copy.size(); ++i) {
        assert(&copy.at(i) != &original.at(i));
        // Deep: the strings themselves are separate objects too.
        assert(copy.at(i).content().data() != original.at(i).content().data());
    }

    // Mutating the original (including a reallocation) leaves the copy intact.
    for (int i = 0; i < 20; ++i) original.append(Message(Role::User, "extra"));
    assert(copy.size() == 5);
    assert(copy.at(4).content() == "message number 4");

    // Assignment must replace the target without sharing the source buffer.
    Conversation target = make_conversation(3);
    target = copy;
    assert(target.begin() != copy.begin());
    assert(same_messages(target, copy));

    // Self-assignment is safe and preserves contents.
    Conversation& alias = target;
    target = alias;
    assert(same_messages(target, copy));

    // Copying an empty conversation yields an empty conversation.
    Conversation empty;
    Conversation empty_copy(empty);
    assert(empty_copy.size() == 0 && empty_copy.begin() == empty_copy.end());
    target = empty;
    assert(target.size() == 0);

    // Scope exit destroys original, copy, target independently: a shallow
    // copy would double-free here and ASan would abort.
}

// Spec item 4: move ctor / move assignment steal the pointer and zero the source.
TEST(RuleOfFiveMoveSteals) {
    Conversation source = make_conversation(7);
    const Message* buffer = source.begin();
    const std::size_t cap = source.capacity();

    // Move constructor.
    Conversation stolen(std::move(source));
    assert(stolen.begin() == buffer && "move must steal, not copy");
    assert(stolen.size() == 7);
    assert(stolen.capacity() == cap);
    assert(stolen.at(6).content() == "message number 6");

    assert(source.size() == 0);             // NOLINT(bugprone-use-after-move)
    assert(source.capacity() == 0);
    assert(source.begin() == nullptr);
    assert(source.begin() == source.end());

    // Moved-from object is valid and reusable.
    source.append(Message(Role::User, "reborn"));
    assert(source.size() == 1 && source.at(0).content() == "reborn");
    assert(source.begin() != buffer);

    // Move assignment into a non-empty target: target's old buffer is freed
    // (ASan leak check), pointer is stolen, source zeroed.
    Conversation target = make_conversation(3);
    target = std::move(stolen);
    assert(target.begin() == buffer);
    assert(target.size() == 7);
    assert(stolen.size() == 0 && stolen.capacity() == 0 && stolen.begin() == nullptr);

    // Self-move-assignment leaves the object intact.
    Conversation& alias = target;
    target = std::move(alias);
    assert(target.begin() == buffer && target.size() == 7);

    // Moving an empty conversation is fine.
    Conversation empty;
    Conversation also_empty(std::move(empty));
    assert(also_empty.size() == 0 && also_empty.begin() == nullptr);

    // Move operations are noexcept (required for the spec's guarantee).
    static_assert(std::is_nothrow_move_constructible_v<Conversation>);
    static_assert(std::is_nothrow_move_assignable_v<Conversation>);
}

// Spec item 5: capacity follows the documented x2 growth factor, and
// size()/at() stay correct across every reallocation.
TEST(GrowthDoublesAndPreservesContents) {
    Conversation conv;
    assert(conv.capacity() == 0);

    std::size_t expected_capacity = 0;
    std::size_t reallocations = 0;
    const Message* last_buffer = conv.begin();

    const std::size_t n = 5000;
    for (std::size_t i = 0; i < n; ++i) {
        if (i == expected_capacity) {
            expected_capacity = expected_capacity == 0 ? 1 : expected_capacity * 2;
        }
        conv.append(Message(Role::User, "m" + std::to_string(i)));

        assert(conv.size() == i + 1);
        assert(conv.capacity() == expected_capacity && "capacity must double");
        assert(conv.capacity() >= conv.size());

        if (conv.begin() != last_buffer) {
            ++reallocations;
            last_buffer = conv.begin();
            // Right after a reallocation, every earlier element must have
            // been carried over intact.
            for (std::size_t j = 0; j <= i; ++j) {
                assert(conv.at(j).content() == "m" + std::to_string(j));
            }
        }
    }

    // 1, 2, 4, ..., 4096, 8192 -> 14 allocations for 5000 appends: O(log n).
    assert(reallocations == 14);
    assert(conv.capacity() == 8192);

    // Iteration visits exactly size() elements, oldest first.
    std::size_t idx = 0;
    for (const Message& m : conv) {
        assert(m.content() == "m" + std::to_string(idx));
        ++idx;
    }
    assert(idx == n);
    assert(static_cast<std::size_t>(conv.end() - conv.begin()) == n);
}

// SentinelScanner

// Spec item 6: text with no sentinel passes through unchanged.
TEST(ScannerCleanText) {
    // Plain text with no '<' is emitted immediately — nothing held back.
    {
        SentinelScanner scanner(kSentinel);
        auto out = scanner.feed("Hello, world! How are you?");
        assert(out.safe_text == "Hello, world! How are you?");
        assert(!out.sentinel_found);
        assert(scanner.pending_size() == 0);
        auto fl = scanner.flush();
        assert(fl.safe_text.empty() && !fl.sentinel_found);
    }

    // Many chunks concatenate back to exactly the input.
    {
        SentinelScanner scanner(kSentinel);
        std::string all;
        for (const char* piece : {"The quick ", "brown fox ", "", "jumps over", " the lazy dog."}) {
            auto out = scanner.feed(piece);
            assert(!out.sentinel_found);
            all += out.safe_text;
        }
        all += scanner.flush().safe_text;
        assert(all == "The quick brown fox jumps over the lazy dog.");
    }

    // Empty stream: nothing out, nothing found.
    {
        SentinelScanner scanner(kSentinel);
        auto out = scanner.feed("");
        assert(out.safe_text.empty() && !out.sentinel_found);
        auto fl = scanner.flush();
        assert(fl.safe_text.empty() && !fl.sentinel_found);
    }

    // Clean text containing '<' and HTML-ish tags survives every chunk size.
    const std::string text = "a < b, <b>bold</b>, x<|y, <|end, <|end_ and done";
    for (std::size_t piece = 1; piece <= text.size(); ++piece) {
        bool found = true;
        assert(scan_in_pieces(text, piece, found) == text);
        assert(!found);
    }

    // An empty sentinel is rejected up front.
    bool threw = false;
    try { SentinelScanner bad(""); } catch (const std::invalid_argument&) { threw = true; }
    assert(threw);
}

// Spec item 7: the sentinel is caught at every possible split point.
TEST(ScannerCatchesSentinelAtEveryBoundary) {
    const std::string text = "Goodbye." + kSentinel;

    // Every single split point (the spec's sample test).
    for (std::size_t split = 0; split <= text.size(); ++split) {
        SentinelScanner scanner(kSentinel);
        auto out1 = scanner.feed(text.substr(0, split));
        auto out2 = scanner.feed(text.substr(split));
        assert((out1.sentinel_found || out2.sentinel_found) &&
               "sentinel must be caught regardless of split point");
        assert(out1.safe_text + out2.safe_text == "Goodbye.");
        // Never a false early report: out1 can only report found if it
        // actually contained the whole sentinel.
        assert(out1.sentinel_found == (split == text.size()));
    }

    // Every pair of split points (three chunks).
    for (std::size_t a = 0; a <= text.size(); ++a) {
        for (std::size_t b = a; b <= text.size(); ++b) {
            SentinelScanner scanner(kSentinel);
            auto o1 = scanner.feed(text.substr(0, a));
            auto o2 = scanner.feed(text.substr(a, b - a));
            auto o3 = scanner.feed(text.substr(b));
            assert(o1.sentinel_found || o2.sentinel_found || o3.sentinel_found);
            assert(o1.safe_text + o2.safe_text + o3.safe_text == "Goodbye.");
        }
    }

    // One character at a time: found exactly on the final '>' and not before.
    {
        SentinelScanner scanner(kSentinel);
        std::string safe;
        for (std::size_t i = 0; i < text.size(); ++i) {
            auto out = scanner.feed(text.substr(i, 1));
            safe += out.safe_text;
            assert(out.sentinel_found == (i == text.size() - 1));
        }
        assert(safe == "Goodbye.");
    }

    // Sentinel preceded by a near-miss prefix, at every chunk size.
    const std::string tricky = "a<|end_<|end_conver" + kSentinel + "TRAILING JUNK";
    for (std::size_t piece = 1; piece <= tricky.size(); ++piece) {
        bool found = false;
        assert(scan_in_pieces(tricky, piece, found) == "a<|end_<|end_conver");
        assert(found);
    }

    // Anything after the sentinel is discarded, and the scanner stays latched.
    {
        SentinelScanner scanner(kSentinel);
        auto out = scanner.feed("Bye." + kSentinel + "should vanish");
        assert(out.sentinel_found && out.safe_text == "Bye.");
        auto later = scanner.feed("more text");
        assert(later.sentinel_found && later.safe_text.empty());
        auto fl = scanner.flush();
        assert(fl.sentinel_found && fl.safe_text.empty());
    }
}

// Spec item 8: partial matches never trigger the sentinel.
TEST(ScannerIgnoresFalseAlarms) {
    const std::string near_misses[] = {
        "<|end_world|>",
        "<|end_conversation|",           // missing final '>'
        "<|end_conversation>",           // missing '|'
        "<|end_conversatio|>",           // one letter short
        "<|END_CONVERSATION|>",          // wrong case
        "< |end_conversation|>",         // extra space
        "<|end_conversation |>",
        "<|end_<|end_<|end_<|end_",
        "<<<<<<<<<<<<<<<<<<<<<<<<",
        "text ending in a prefix <|end_convers",
    };
    for (const std::string& s : near_misses) {
        for (std::size_t piece = 1; piece <= s.size(); ++piece) {
            bool found = true;
            const std::string safe = scan_in_pieces(s, piece, found);
            assert(!found && "near miss must not be reported as the sentinel");
            assert(safe == s && "near miss text must be released unchanged");
        }
    }

    // A held-back prefix that turns out not to be the sentinel is released
    // on the very next feed, not delayed until flush.
    SentinelScanner scanner(kSentinel);
    auto o1 = scanner.feed("Hi <|end_");
    assert(o1.safe_text == "Hi " && !o1.sentinel_found);
    assert(scanner.pending_size() == 6);
    auto o2 = scanner.feed("world|> ok");
    assert(o2.safe_text == "<|end_world|> ok" && !o2.sentinel_found);
    assert(scanner.pending_size() == 0);
}

// Spec item 9: pending_ never exceeds sentinel.size() - 1, even when a 4 MB
// adversarial stream arrives one byte at a time.
TEST(ScannerBoundedMemoryUnderAdversarialStream) {
    const std::size_t bound = kSentinel.size() - 1;
    const std::size_t target_bytes = 4u * 1024u * 1024u;

    // Patterns chosen to keep the scanner holding back as much as possible:
    // the longest proper prefix (19 chars), repeated short prefixes, and
    // runs of '<'.
    const std::string patterns[] = {
        "<|end_conversation|",
        "<|end_<|end_<|end_",
        "<<<<<<",
        "<|end_conversatio",
        "x",
    };
    std::string stream;
    stream.reserve(target_bytes + 64);
    for (std::size_t i = 0; stream.size() < target_bytes; ++i) {
        stream += patterns[i % 5];
    }
    assert(stream.find(kSentinel) == std::string::npos);

    SentinelScanner scanner(kSentinel);
    std::size_t emitted_bytes = 0;
    std::size_t max_pending = 0;
    bool matches_input = true;
    for (std::size_t i = 0; i < stream.size(); ++i) {
        auto out = scanner.feed(std::string_view(&stream[i], 1));
        assert(!out.sentinel_found);
        // Emitted text must be exactly the next bytes of the input.
        if (stream.compare(emitted_bytes, out.safe_text.size(), out.safe_text) != 0) {
            matches_input = false;
        }
        emitted_bytes += out.safe_text.size();
        const std::size_t pending = scanner.pending_size();
        assert(pending <= bound && "pending_ exceeded sentinel.size() - 1");
        // Everything fed is either emitted or pending — nothing lost.
        assert(emitted_bytes + pending == i + 1);
        if (pending > max_pending) max_pending = pending;
    }
    assert(matches_input);
    assert(max_pending == bound && "stream should have pushed pending_ to the bound");

    auto fl = scanner.flush();
    emitted_bytes += fl.safe_text.size();
    assert(emitted_bytes == stream.size());
    assert(scanner.pending_size() == 0);

    // Same stream with the real sentinel on the end: found on the last byte.
    SentinelScanner scanner2(kSentinel);
    const std::string with_sentinel = stream + kSentinel;
    bool found = false;
    for (std::size_t i = 0; i < with_sentinel.size(); ++i) {
        auto out = scanner2.feed(std::string_view(&with_sentinel[i], 1));
        assert(scanner2.pending_size() <= bound);
        if (out.sentinel_found) {
            assert(i == with_sentinel.size() - 1);
            found = true;
        }
    }
    assert(found);
}

// Provided Harness running on top of Conversation + SentinelScanner

// Spec item 10: the loop stops with TurnLimit after max_turns completed turns.
TEST(HarnessStopsAtTurnLimit) {
    const std::string script = write_file("turn_limit.script",
        "chunk: 4\nrole: assistant\nReply one.\n---\n"
        "role: assistant\nReply two.\n---\n"
        "role: assistant\nReply three.\n---\n"
        "role: assistant\nReply four.\n---\n"
        "role: assistant\nReply five.\n");

    HarnessConfig cfg;
    cfg.max_turns = 3;
    Harness harness(std::make_unique<ScriptedModelClient>(script), cfg);
    FakeInput in({"q1", "q2", "q3", "q4", "q5"});
    CaptureOutput out;

    const StopReason reason = harness.run(in, out);
    assert(reason.kind == StopReason::Kind::TurnLimit);
    assert(in.remaining() == 2 && "loop must not read input past the limit");

    const Conversation& conv = harness.conversation();
    assert(conv.size() == 6);  // 3 x (user, assistant)
    assert(conv.at(0).role() == Role::User && conv.at(0).content() == "q1");
    assert(conv.at(1).role() == Role::Assistant && conv.at(1).content() == "Reply one.");
    assert(conv.at(5).content() == "Reply three.");
    assert(out.text() ==
           "you> assistant> Reply one.\n"
           "you> assistant> Reply two.\n"
           "you> assistant> Reply three.\n");

    // Blank lines are re-prompted and do not count as turns.
    Harness blanks(std::make_unique<ScriptedModelClient>(script), cfg);
    FakeInput in2({"", "q1", "", "", "q2", "q3"});
    CaptureOutput out2;
    assert(blanks.run(in2, out2).kind == StopReason::Kind::TurnLimit);
    assert(blanks.conversation().size() == 6);

    // --max-turns 0 stops immediately without reading input.
    HarnessConfig zero;
    zero.max_turns = 0;
    Harness none(std::make_unique<ScriptedModelClient>(script), zero);
    FakeInput in3({"q1"});
    CaptureOutput out3;
    assert(none.run(in3, out3).kind == StopReason::Kind::TurnLimit);
    assert(none.conversation().size() == 0 && in3.remaining() == 1);
}

// Spec item 11: the loop halts exactly on the turn where the scanner reports
// the sentinel, never prints it, but stores it for transcript replay.
TEST(HarnessHaltsOnSentinel) {
    // Chunk sizes 1..25 cover every way the sentinel can be split.
    for (int chunk = 1; chunk <= 25; ++chunk) {
        const std::string script = write_file("sentinel.script",
            "chunk: " + std::to_string(chunk) + "\nrole: assistant\nHi there!\n---\n"
            "chunk: " + std::to_string(chunk) + "\nrole: assistant\n"
            "Goodbye.<|end_conversation|>ignored tail\n---\n"
            "role: assistant\nThis reply must never be requested.\n");

        Harness harness(std::make_unique<ScriptedModelClient>(script), HarnessConfig{});
        FakeInput in({"hello", "bye", "are you still there?"});
        CaptureOutput out;

        const StopReason reason = harness.run(in, out);
        assert(reason.kind == StopReason::Kind::Sentinel);
        assert(reason.detail == "stop sentinel after 2 turns");
        assert(in.remaining() == 1 && "loop must stop right after the sentinel turn");

        // Terminal output: sentinel and anything after it are hidden.
        assert(out.text() == "you> assistant> Hi there!\nyou> assistant> Goodbye.\n");
        assert(out.text().find("<|end") == std::string::npos);

        // Conversation keeps the sentinel (so replay re-triggers the stop)
        // but drops what came after it.
        const Conversation& conv = harness.conversation();
        assert(conv.size() == 4);
        assert(conv.at(1).content() == "Hi there!");
        assert(conv.at(3).role() == Role::Assistant);
        assert(conv.at(3).content() == "Goodbye." + kSentinel);
    }
}

// Rubric: EOF (Ctrl-D) ends the loop gracefully with UserExit, keeping history.
TEST(HarnessHandlesEofAndClientError) {
    const std::string script = write_file("eof.script",
        "role: system\nBe brief.\n---\nrole: assistant\nSure.\n---\n"
        "role: assistant\nOkay.\n");

    // EOF after one turn.
    auto model = std::make_unique<ScriptedModelClient>(script);
    HarnessConfig cfg;
    cfg.system_message = model->system_message();
    Harness harness(std::move(model), cfg);
    FakeInput in({"first"});
    CaptureOutput out;
    const StopReason reason = harness.run(in, out);
    assert(reason.kind == StopReason::Kind::UserExit);
    assert(harness.conversation().size() == 3);  // system, user, assistant
    assert(harness.conversation().at(2).content() == "Sure.");

    // EOF on the very first prompt: only the system message remains.
    Harness immediate(std::make_unique<ScriptedModelClient>(script), cfg);
    FakeInput empty_in({});
    CaptureOutput out2;
    assert(immediate.run(empty_in, out2).kind == StopReason::Kind::UserExit);
    assert(immediate.conversation().size() == 1);

    // Script exhaustion surfaces as ClientError.
    Harness exhausted(std::make_unique<ScriptedModelClient>(script), cfg);
    FakeInput many({"a", "b", "c"});
    CaptureOutput out3;
    const StopReason err = exhausted.run(many, out3);
    assert(err.kind == StopReason::Kind::ClientError);
    assert(exhausted.conversation().size() == 6);  // sys + 2 full turns + dangling user
}

// Spec item 12: save a session, replay it via ReplayModelClient, and get the
// identical session back.
TEST(TranscriptRoundTrip) {
    const std::string script = write_file("roundtrip.script",
        "role: system\nBe concise.\n---\n"
        "chunk: 5\nrole: assistant\nI am doing well, thank you! How can I help you?\n---\n"
        "chunk: 7\nrole: assistant\nLine one of a reply.\nLine two of a reply.\n---\n"
        "chunk: 6\nrole: assistant\nGoodbye!<|end_conversation|>\n");
    const std::deque<std::string> user_lines = {"hello", "tell me two lines", "bye"};

    // Session 1: live scripted model, saved to disk.
    auto scripted = std::make_unique<ScriptedModelClient>(script);
    HarnessConfig cfg1;
    cfg1.system_message = scripted->system_message();
    Harness first(std::move(scripted), cfg1);
    FakeInput in1(user_lines);
    CaptureOutput out1;
    const StopReason r1 = first.run(in1, out1);
    assert(r1.kind == StopReason::Kind::Sentinel);

    const std::string transcript = (test_dir() / "roundtrip_transcript.txt").string();
    save_transcript(first.conversation(), transcript);

    // Session 2: replay the saved transcript with the same user input.
    auto replay = std::make_unique<ReplayModelClient>(transcript);
    HarnessConfig cfg2;
    cfg2.system_message = replay->system_message();
    assert(cfg2.system_message == "Be concise.");
    Harness second(std::move(replay), cfg2);
    FakeInput in2(user_lines);
    CaptureOutput out2;
    const StopReason r2 = second.run(in2, out2);

    // Identical playback: same stop, same terminal output, same history.
    assert(r2.kind == r1.kind && r2.detail == r1.detail);
    assert(out2.text() == out1.text());
    assert(same_messages(second.conversation(), first.conversation()));
    assert(second.conversation().size() == 7);
    assert(second.conversation().at(4).content() == "Line one of a reply.\nLine two of a reply.");

    // Saving the replayed session reproduces the transcript byte-for-byte.
    const std::string transcript2 = (test_dir() / "roundtrip_transcript2.txt").string();
    save_transcript(second.conversation(), transcript2);
    assert(read_file(transcript2) == read_file(transcript));

    // The non-streaming NVI generate() returns each recorded reply verbatim.
    ReplayModelClient direct(transcript);
    Conversation dummy;
    assert(direct.generate(dummy).content() == "I am doing well, thank you! How can I help you?");
    assert(direct.generate(dummy).content() == "Line one of a reply.\nLine two of a reply.");
    assert(direct.generate(dummy).content() == "Goodbye!" + kSentinel);
}


TEST(AppendOwnElementDuringGrowth) {
    Conversation conv;
    conv.append(Message(Role::User, std::string(200, 'x')));
    for (int i = 0; i < 100; ++i) conv.append(conv.at(0));
    assert(conv.size() == 101);
    for (const auto& msg : conv) assert(msg.content() == std::string(200, 'x'));
}

TEST(ScannerMatchesWholeStringSearch) {
    std::mt19937 rng(309);
    const std::string markers[] = {"#", "aaa", "abab", "aab", kSentinel,
                                   std::string("a\0b", 3)};
    for (const auto& marker : markers) {
        for (int trial = 0; trial < 300; ++trial) {
            std::string text;
            for (int i = 0; i < 80; ++i) text += "ab#<>\0"[rng() % 6];
            if (trial % 2 == 0) text.insert(rng() % (text.size() + 1), marker);
            const auto pos = text.find(marker);
            SentinelScanner scanner(marker);
            std::string output;
            bool found = false;
            for (std::size_t i = 0; i < text.size();) {
                const auto empty = scanner.feed({});
                assert(empty.safe_text.empty());
                assert(empty.sentinel_found == found);
                const std::size_t len = std::min<std::size_t>(1 + rng() % 12, text.size() - i);
                auto out = scanner.feed(std::string_view(text).substr(i, len));
                output += out.safe_text;
                found = out.sentinel_found;
                assert(scanner.pending_size() < marker.size());
                i += len;
            }
            output += scanner.flush().safe_text;
            assert(scanner.flush().safe_text.empty());
            assert(found == (pos != std::string::npos));
            assert(output == text.substr(0, pos));
        }
    }
}

int main() {
    for (std::size_t i = 0; i < g_test_count; ++i) {
        std::cout << "[ RUN  ] " << g_tests[i].name << std::endl;
        g_tests[i].fn();
        std::cout << "[  OK  ] " << g_tests[i].name << std::endl;
    }
    std::filesystem::remove_all(test_dir());
    std::cout << "\nAll " << g_test_count << " tests passed.\n";
    return 0;
}
