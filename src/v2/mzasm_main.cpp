// mzasm_main.cpp (maize-422): the command-line surface of the Maize v2 assembler.
//
// D-5 keeps this surface and the diagnostic line shape the same as v1 mazm's, so the VS Code
// tasks can reuse one problemMatcher pattern rather than growing a second, and so anything that
// already drives mazm from an editor drives mzasm the same way.
//
// D-8 fixes what touches the filesystem. --check runs the whole pipeline with no filesystem
// effect at all, writing nothing and removing nothing. An ordinary run removes a stale output
// sitting at the target path BEFORE assembly begins, so a failed assembly can never leave a
// previously good binary behind looking current; that is v1 mazm's own rule and the reasoning
// carries over unchanged.
//
// D-9 fixes the flat-mode suffix at .mzi, and v1 keeps its own. Two machines whose images are
// indistinguishable by name invite feeding one to the other, and neither loader inspects a file
// before loading it, so the naming is the only place a reader catches the mistake. A suffix is a
// convention rather than a check, and this buys a naming-level mistake in place of a
// content-level one, which is all it claims.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "mzasm.h"

namespace {

void print_usage(std::ostream& out) {
    out << "usage: mzasm [options] <input.mzasm>\n"
           "\n"
           "Maize v2 assembler. Assembles <input.mzasm> into a flat memory image written\n"
           "next to the input file as <input>.mzi, or into a relocatable object\n"
           "<input>.mzo with -c. On assembly errors no output is produced and any\n"
           "stale output at the target path is removed.\n"
           "\n"
           "options:\n"
           "  -c, --emit-object     emit a relocatable .mzo object instead of a flat .mzi\n"
           "  --check               validate only: run the full assembly pipeline with\n"
           "                        no filesystem effects (nothing written or removed)\n"
           "  --stdin               read source from standard input instead of a file;\n"
           "                        requires --base-path, plus either --check (validate\n"
           "                        only) or -c (emit <base-path>/<source-name>.mzo)\n"
           "  --base-path <dir>     directory include paths resolve against\n"
           "                        (default: the input file's directory)\n"
           "  --source-name <name>  name reported in diagnostics for --stdin input\n"
           "                        (default: <stdin>)\n"
           "  -h, --help            show this help and exit\n"
           "\n"
           "Unrecognized --flags are ignored, so editor integrations can pass newer\n"
           "flags to older assemblers.\n";
}

bool write_file(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

}  // namespace

int main(int argc, char* argv[]) {
    using maize::v2::asmr::Assembler;
    using maize::v2::asmr::PlacementMode;

    // Flags are position-independent and the first non-flag argument is the input file.
    // Unrecognized --flags are ignored rather than fatal, so a newer editor integration can
    // pass a flag an older assembler does not know.
    bool check_only = false;
    bool emit_object = false;
    bool stdin_mode = false;
    std::string input_file;
    std::string base_path_arg;
    std::string source_name = "<stdin>";

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            print_usage(std::cout);
            return 0;
        } else if (arg == "--check") {
            check_only = true;
        } else if (arg == "-c" || arg == "--emit-object") {
            emit_object = true;
        } else if (arg == "--stdin") {
            stdin_mode = true;
        } else if (arg == "--base-path" && i + 1 < argc) {
            base_path_arg = argv[++i];
        } else if (arg == "--source-name" && i + 1 < argc) {
            source_name = argv[++i];
        } else if (arg.rfind("--", 0) == 0) {
            // ignored
        } else if (input_file.empty()) {
            input_file = arg;
        }
    }

    std::filesystem::path output_path;
    Assembler assembler;
    bool ok = false;

    if (stdin_mode) {
        if (!check_only && !emit_object) {
            std::cerr << "mzasm: error: --stdin requires --check or -c/--emit-object\n";
            return 1;
        }
        if (base_path_arg.empty()) {
            std::cerr << "mzasm: error: --stdin requires --base-path\n";
            return 1;
        }
        std::string text((std::istreambuf_iterator<char>(std::cin)),
                         std::istreambuf_iterator<char>());
        if (emit_object && !check_only) {
            output_path = std::filesystem::path(base_path_arg) / (source_name + ".mzo");
            std::error_code ec;
            std::filesystem::remove(output_path, ec);
        }
        ok = assembler.assemble_text(text, source_name, base_path_arg);
    } else {
        if (input_file.empty()) {
            // No input and no --stdin: print help and exit nonzero, so a bare or misspelled
            // invocation is never mistaken for success.
            print_usage(std::cerr);
            return 1;
        }
        if (!std::filesystem::exists(input_file)) {
            std::cerr << "mzasm: error: cannot read '" << input_file << "'\n";
            return 1;
        }
        if (!check_only) {
            output_path = std::filesystem::path(input_file);
            output_path.replace_extension(emit_object ? "mzo" : "mzi");
            std::error_code ec;
            std::filesystem::remove(output_path, ec);
        }
        ok = assembler.assemble_file(input_file);
    }

    if (!ok) {
        std::cerr << assembler.diagnostics().format();
        return 1;
    }

    if (check_only) {
        return 0;  // the full pipeline ran and touched nothing
    }

    if (emit_object) {
        if (assembler.mode() != PlacementMode::Sectioned) {
            std::cerr << "mzasm: error: -c emits a relocatable object, and this module declares "
                         "no section\n";
            return 1;
        }
        if (!write_file(output_path, assembler.serialize_object())) {
            std::cerr << "mzasm: error: cannot write '" << output_path.string() << "'\n";
            return 1;
        }
    } else {
        if (assembler.mode() == PlacementMode::Sectioned) {
            std::cerr << "mzasm: error: this module declares sections, so it assembles to a "
                         "relocatable object; pass -c\n";
            return 1;
        }
        if (!write_file(output_path, assembler.flat_image())) {
            std::cerr << "mzasm: error: cannot write '" << output_path.string() << "'\n";
            return 1;
        }
    }

    std::cout << "Output to " << output_path.string() << std::endl;
    return 0;
}
