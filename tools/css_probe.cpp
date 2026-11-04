#include "css.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <random>
#include <string>
using namespace pico_page::document::css;
int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--fuzz-seconds") {
        const auto start = std::chrono::steady_clock::now();
        const auto seconds = std::stoi(argv[2]);
        std::mt19937 random(108);
        const std::string seed = "p.x > div, #a{font-size:1.2em;margin:2% 3px!important}@media print{p{display:none}}p{unknown:url(art/*)}";
        uint64_t runs = 0;
        do {
            std::string input = seed;
            for (unsigned n = 0, changes = random() % 32 + 1; n < changes; ++n) {
                const auto index = random() % (input.size() + 1);
                if (index == input.size()) input.push_back(static_cast<char>(random()));
                else input[index] = static_cast<char>(random());
            }
            StyleSheet sheet;
            sheet.append(input);
            WorkBudget budget; budget.maximum_work = 10000;
            ComputedStyle result;
            Element root{"div", "a", "x"}, leaf{"p", "", "x", &root};
            sheet.compute(leaf, nullptr, {}, input.substr(0, 100), budget, result);
            if (budget.work > budget.maximum_work) return 2;
            ++runs;
        } while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds));
        std::cout << "{\"runs\":" << runs << ",\"seconds\":" << seconds << "}\n";
        return 0;
    }
    if (argc != 2) return 2;
    std::ifstream file;
    if (std::string(argv[1]) != "--stdin") { file.open(argv[1], std::ios::binary); if (!file) return 2; }
    std::istream& stream = file.is_open() ? file : std::cin;
    std::string source((std::istreambuf_iterator<char>(stream)), {});
    StyleSheet sheet;
    const auto result = sheet.append(source);
    const auto report = sheet.report();
    std::cout << "{\"result\":" << static_cast<unsigned>(result)
              << ",\"source_bytes\":" << report.source_bytes << ",\"rules\":" << report.rules
              << ",\"selectors\":" << report.selectors << ",\"declarations\":" << report.declarations
              << ",\"rejected_rules\":" << report.rejected_rules << ",\"rejected_declarations\":" << report.rejected_declarations
              << ",\"skipped_at_rules\":" << report.skipped_at_rules << ",\"allocated_bytes\":" << report.allocated_bytes << "}\n";
}
