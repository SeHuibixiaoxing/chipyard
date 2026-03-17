#include <cstdio>
#include <string>
#include <cassert>
#include "yaml-cpp/yaml.h"


void printType(YAML::Node &node) {
    printf("type: map=%d, seq=%d, scalar=%d, size=%lu\n",
           node.IsMap(), node.IsSequence(), node.IsScalar(), node.size());
}

void test0();


int main() {
    test0();

    return 0;
}


void test0() {
    std::string path = "test/input_test_yaml/1.yaml";

    YAML::Node node = YAML::LoadFile(path);
    printType(node);

    YAML::Node node1 = node["key"];
    printType(node1);

    YAML::Node node10 = node1[0];
    printType(node10);
    YAML::Node node100 = node10["a"];
    printType(node100);
    printf("%d\n", node100.as<int>());
    YAML::Node node101 = node10["b"];
    printType(node101);
    printf("%s\n", node101.as<std::string>().c_str());

    YAML::Node node11 = node1[1];
    printType(node11);
    YAML::Node node110 = node11["c"];
    printType(node110);
    auto v = node110.as<std::vector<int>>();
    for (uint i = 0; i < v.size(); i++) {
        printf("%d,", i);
    }
    printf("\n");

    YAML::Node node12 = node1[2];
    printType(node12);
    YAML::Node node120 = node12["d"];
    printType(node120);
    auto vv = node120.as<std::vector<std::vector<int>>>();
    for (auto &v_ : vv) {
        for (auto i : v_) {
            printf("%d,", i);
        }
        printf("\n");
    }
    printf("\n");

//    printf("%d\n", node["key"].as<int>());
}
