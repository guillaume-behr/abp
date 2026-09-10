#include <iostream>

#include "TestFramework.h"

int main() {
    int failed = 0;
    for (const auto& t : abp::test::registry()) {
        try {
            t.fn();
            std::cout << "[PASS] " << t.name << "\n";
        } catch (const abp::test::AssertionFailure& f) {
            std::cout << "[FAIL] " << t.name << ": " << f.message << "\n";
            ++failed;
        } catch (const std::exception& e) {
            std::cout << "[FAIL] " << t.name << ": unexpected exception: " << e.what() << "\n";
            ++failed;
        }
    }
    std::cout << abp::test::registry().size() << " test(s), " << failed << " failed.\n";
    return failed == 0 ? 0 : 1;
}
