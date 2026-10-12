#include "common.hpp"
int main(int argc, char **argv) {
    try {
        ft::Cases cases;
        ct::pipeline_cases(cases);
        ct::pool_cases(cases);
        ct::block_alloc_cases(cases);
        ct::block_pool_cases(cases);
        ct::queue_cases(cases);
        std::sort(cases.begin(), cases.end(), [](auto &a, auto &b) { return a.id < b.id; });
        for (std::size_t i = 1; i < cases.size(); ++i)
            CHECK(cases[i - 1].id != cases[i].id);
        if (argc == 2 && std::string(argv[1]) == "--list") {
            for (auto &c : cases)
                std::cout << c.id << '\n';
            return 0;
        }
        if (argc != 3 || std::string(argv[1]) != "--case")
            throw std::runtime_error("usage: --case ID | --list");
        for (auto &c : cases)
            if (c.id == argv[2]) {
                c.run();
                std::cout << "PASS " << c.id << '\n';
                return 0;
            }
        throw std::runtime_error("unknown case");
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << (argc > 2 ? argv[2] : "") << " " << e.what() << '\n';
        return 1;
    }
}
