// mzvm_options.h (maize-467): the numeric-option parser mzvm's command line runs every value
// through.
//
// This lives in a header of its own rather than inside mzvm_main.cpp for one reason: the defect
// it exists to prevent is about which BYTES may appear in front of a value, and the fixture that
// proves it walks all 256 of them. A fixture driving the shipped binary cannot do that, because
// it reaches the binary through a shell and a shell will not carry a raw newline inside an
// argument on every host this project builds on. The end-to-end fixture still asserts the
// diagnostics and the exit status against the real binary; this header is what lets the byte
// range be swept directly.

#ifndef MAIZE_V2_MZVM_OPTIONS_H
#define MAIZE_V2_MZVM_OPTIONS_H

#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace maize::v2 {

// Parse one numeric option value, refusing anything this machine cannot honour.
//
// The rule every option here shares is that a value the loader cannot use is REJECTED, never
// quietly replaced by one it can. std::strtoull breaks that rule twice on its own: it saturates
// at ULLONG_MAX and sets ERANGE when the text names something larger, and it negates a leading
// minus and reports nothing at all, so `-1` arrives as the highest address there is. A caller
// that inspects only the end pointer accepts both and silently loads somewhere the user did not
// ask for. The machine this loader serves traps on a reserved paging-root mode and faults on an
// unimplemented CSR number rather than reinterpreting either, and a loader that clamps disagrees
// with the machine it loads for.
//
// The diagnostic is written here rather than by the caller so that every option reports a
// rejection the same way, naming the option, the text it refused, and the range it accepts. It
// goes to the stream the caller names so that a fixture sweeping thousands of rejections can
// send them somewhere other than the console.
//
// The program name arrives as an argument rather than being written into the sentences here
// (maize-456), because two binaries run this parser: mzvm and its graphical twin mzvmg, built
// from the same translation unit under different names. A literal here would make one of them
// lie about which program refused the value. The caller passes its own compile-time name, and
// the in-process fixtures pass the name whose behaviour they are asserting.
inline bool parse_number(const char* program_name, const char* option, const char* expected,
                         const char* text, std::uint64_t minimum, std::uint64_t maximum,
                         std::uint64_t& out, std::FILE* diagnostics = stderr) {
    if (text == nullptr || *text == '\0') {
        std::fprintf(diagnostics, "%s: %s needs %s\n", program_name, option, expected);
        return false;
    }

    // A leading minus is caught before the conversion, because the conversion does not report it.
    // Leading whitespace has to be stepped over first, since the sign can sit behind it.
    //
    // THE TWO SCANS MUST AGREE ON WHERE THE VALUE STARTS, and the way to get that is to leave
    // them no room to disagree rather than to write the same rule out twice. A hand-listed set
    // here is what failed the first time this was written: it stepped over a space and a tab
    // only, strtoull steps over the whole isspace set, and a minus behind a newline therefore
    // walked past the guard and was negated by the conversion with nothing reported. So the skip
    // below uses std::isspace, which is the predicate strtoull's own contract is written in terms
    // of, and the conversion then starts from `first`, the pointer this scan stopped at, rather
    // than from `text`. The second of those is what makes the agreement structural: whatever
    // isspace answers, strtoull begins where this loop ended, so no character class can be
    // stepped over by one scan and not the other.
    //
    // isspace is fed an unsigned char because it is undefined on a negative value other than
    // EOF, and a plain char is signed on this host.
    const char* first = text;
    while (std::isspace(static_cast<unsigned char>(*first)) != 0) {
        ++first;
    }
    if (*first == '-') {
        std::fprintf(diagnostics, "%s: %s needs %s, and '%s' is negative\n", program_name, option,
                     expected, text);
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long value = std::strtoull(first, &end, 0);
    if (end == first || *end != '\0') {
        std::fprintf(diagnostics, "%s: %s needs %s, and '%s' is not a number\n", program_name,
                     option, expected, text);
        return false;
    }
    // Both tests are load-bearing and neither subsumes the other. ERANGE is the only signal when
    // the option's own ceiling is the whole width of the type, since the saturated result is a
    // legal value of it; the comparison is the only signal when the option's ceiling is narrower
    // than the type, where the conversion succeeded and reported nothing.
    if (errno == ERANGE || value > maximum || value < minimum) {
        std::fprintf(diagnostics,
                     "%s: %s value '%s' is out of range; the accepted range is %" PRIu64
                     " to %" PRIu64 "\n",
                     program_name, option, text, minimum, maximum);
        return false;
    }
    out = static_cast<std::uint64_t>(value);
    return true;
}

}  // namespace maize::v2

#endif  // MAIZE_V2_MZVM_OPTIONS_H
