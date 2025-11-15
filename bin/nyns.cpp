// C++ port of bin/nyns.sh

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static void interpret_command(const std::string &line) {
    std::istringstream iss(line);
    std::string command_type;
    std::string arg1;
    std::string arg2;

    if (!(iss >> command_type)) {
        return;
    }

    // Mimic: read -r command_type arg1 arg2 <<< "$1"
    iss >> arg1;
    iss >> arg2;

    if (command_type == "echo") {
        if (!arg1.empty()) {
            std::cout << arg1;
        }
        if (!arg2.empty()) {
            if (!arg1.empty()) {
                std::cout << ' ';
            }
            std::cout << arg2;
        }
        std::cout << '\n';
    } else if (command_type == "+") {
        try {
            long long a = std::stoll(arg1);
            long long b = std::stoll(arg2);
            std::cout << (a + b) << '\n';
        } catch (...) {
            std::cerr << "Error: invalid numbers for '+'\n";
        }
    } else if (command_type == "-") {
        try {
            long long a = std::stoll(arg1);
            long long b = std::stoll(arg2);
            std::cout << (a - b) << '\n';
        } catch (...) {
            std::cerr << "Error: invalid numbers for '-'\n";
        }
    } else if (command_type == "rem") {
        // Equivalent to: rm -rf ${arg1}
        if (arg1.empty()) {
            std::cerr << "Error: 'rem' requires a path\n";
            return;
        }
        std::error_code ec;
        fs::remove_all(arg1, ec);
        if (ec) {
            std::cerr << "Error removing '" << arg1 << "': " << ec.message() << '\n';
        }
    } else if (command_type == "moveto") {
        if (arg1.empty()) {
            std::cerr << "Error: 'moveto' requires a directory\n";
            return;
        }
        std::error_code ec;
        fs::current_path(arg1, ec);
        if (ec) {
            std::cerr << "Error changing directory to '" << arg1 << "': " << ec.message() << '\n';
        }
    } else if (command_type == "help") {
        std::cout << "echo: Displays text on-screen\n";
        std::cout << "+: Addition\n";
        std::cout << "-: Removal of number\n";
        std::cout << "rem: Delete a path (irreversible)\n";
        std::cout << "rem arguments: -f: Forced deletion\n";
        std::cout << "moveto: CD into a directory\n";
        std::cout << "help: Get command help\n";
        std::cout << "ip: Get IP address\n";
        std::cout << "create: Create a file\n";
        std::cout << "adm: Run a command as admin\n";
        std::cout << "import: Import a script\n";
    } else if (command_type == "ip") {
        (void)std::system("ip addr");
    } else if (command_type == "create") {
        if (arg1.empty()) {
            std::cerr << "Error: 'create' requires a filename\n";
            return;
        }
        std::ofstream ofs(arg1);
        if (!ofs) {
            std::cerr << "Error creating file '" << arg1 << "'\n";
        }
    } else if (command_type == "adm") {
        if (arg1.empty()) {
            std::cerr << "Error: 'adm' requires a command\n";
            return;
        }
        std::string cmd = "sudo " + arg1;
        (void)std::system(cmd.c_str());
    } else if (command_type == "partition") {
        if (arg1.empty()) {
            std::cerr << "Error: 'partition' requires a device\n";
            return;
        }
        std::string cmd = "fdisk " + arg1;
        (void)std::system(cmd.c_str());
    } else {
        std::cerr << "Error: Unknown command '" << command_type << "'\n";
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <script.nyns>\n";
        return 1;
    }

    const std::string script_path = argv[1];
    std::ifstream in(script_path);
    if (!in) {
        std::cerr << "Error: cannot open '" << script_path << "'\n";
        return 1;
    }

    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        // Skip empty lines and lines starting with '#'
        if (line.empty() || (!line.empty() && line[0] == '#')) {
            continue;
        }
        interpret_command(line);
    }

    return 0;
}
