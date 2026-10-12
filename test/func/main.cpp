#include "support.hpp"
int main(int argc, char **argv) {
    try {
        ft::Cases c;
        ft::write_read_cases(c);
        ft::write_layout_cases(c);
        ft::store_cases(c);
        ft::pool_cases(c);
        ft::concurrency_cases(c);
        ft::death_cases(c);
        ft::policy_cases(c);
        std::sort(c.begin(), c.end(), [](auto &a, auto &b) { return a.id < b.id; });
        for (std::size_t i = 1; i < c.size(); ++i)
            CHECK(c[i - 1].id != c[i].id);
        if (argc == 2 && std::string(argv[1]) == "--list") {
            for (auto &t : c)
                std::cout << t.id << '\n';
            return 0;
        }
        if (argc != 3 || std::string(argv[1]) != "--case")
            throw std::runtime_error("usage: brq_func --case ID | --list");
        for (auto &t : c)
            if (t.id == argv[2]) {
                t.run();
                std::cout << "PASS " << t.id << '\n';
                return 0;
            }
        throw std::runtime_error("unknown case");
    } catch (const std::exception &e) {
        std::cerr << "FAIL " << (argc > 2 ? argv[2] : "") << " " << e.what() << '\n';
        return 1;
    }
}
