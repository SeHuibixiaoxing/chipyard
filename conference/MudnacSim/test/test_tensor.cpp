#include <cstdio>
#include "tensor.h"


using namespace mudnac;


void test_getAddrVector();

void test_getBurstQue();

void test_getBurstQue2();


int main() {

    test_getAddrVector();
    test_getBurstQue();
    test_getBurstQue2();

    return 0;
}


void test_getAddrVector() {
//    mudnac::Tensor tensor(1000, {10});
    mudnac::Tensor tensor(0, {1000, 100, 10, 1});

//    mudnac::Tensor::Vector lo = {5};
//    mudnac::Tensor::Vector hi = {6};
//    mudnac::Tensor::Vector lo = {0};
//    mudnac::Tensor::Vector hi = {10};
//    mudnac::Tensor::Vector lo = {10};
//    mudnac::Tensor::Vector hi = {20};

//    mudnac::Tensor::Vector lo = {0, 0, 0, 10};
//    mudnac::Tensor::Vector hi = {1, 1, 1, 20};
//    mudnac::Tensor::Vector lo = {0, 0, 10, 0};
//    mudnac::Tensor::Vector hi = {1, 1, 20, 1};
//    mudnac::Tensor::Vector lo = {0, 10, 0, 0};
//    mudnac::Tensor::Vector hi = {1, 20, 1, 1};
//    mudnac::Tensor::Vector lo = {10, 0, 0, 0};
//    mudnac::Tensor::Vector hi = {20, 1, 1, 1};
//    mudnac::Tensor::Vector lo = {0, 0, 0, 0};
//    mudnac::Tensor::Vector hi = {2, 2, 2, 2};
    mudnac::Tensor::Vector lo = {2, 2, 2, 2};
    mudnac::Tensor::Vector hi = {4, 4, 4, 4};

    mudnac::Tensor::AddrVector aVec;
    uint64_t bytes = tensor.getAddrVector(lo, hi, aVec);
    printf("%lu\n", bytes);
    for (auto i: aVec) {
        printf("%lu ", i);
    }
    printf("\n");
}


void test_getBurstQue() {
    Tensor::BurstQue bQue;
//    uint64_t maxBurstBytes = 16;
    uint64_t maxBurstBytes = 64;

    Tensor tensor(0, {10, 1});

    mudnac::Tensor::Vector lo = {0, 0};
    mudnac::Tensor::Vector hi = {1, 64};

//    mudnac::Tensor::Vector lo = {0, 13};
//    mudnac::Tensor::Vector hi = {1, 36};

//    mudnac::Tensor::Vector lo = {0, 2};
//    mudnac::Tensor::Vector hi = {10, 4};

//    Tensor tensor(0, {32});
//    mudnac::Tensor::Vector lo = {0};
//    mudnac::Tensor::Vector hi = {16};

//    Tensor tensor(0, {20});

//    mudnac::Tensor::Vector lo = {0};
//    mudnac::Tensor::Vector hi = {1};

//    mudnac::Tensor::Vector lo = {0};
//    mudnac::Tensor::Vector hi = {2};

//    mudnac::Tensor::Vector lo = {1};
//    mudnac::Tensor::Vector hi = {5};

//    Tensor tensor(0, {1, 1, 1, 1});
//    Tensor::Vector lo = {0, 0, 0, 0};
//    Tensor::Vector hi = {1024, 1, 1, 1};

    uint64_t bytes = tensor.getBurstQue(maxBurstBytes, lo, hi, bQue);
    printf("idealBytes=%lu, numBursts=%zu\n", bytes, bQue.size());
    for (auto i: bQue) {
        printf("(%lu,%lu) ", i[0], i[1]);
    }
    printf("\n");
}


void test_getBurstQue2() {
    Tensor::BurstQue bQue;
    std::deque<uint64> spmAddrQue;
    uint64 maxBurstBytes = 16;

    Tensor tensor(0, {100, 1});
    Tensor spmTensor(10000, {10, 1});

//    mudnac::Tensor::Vector lo = {0, 0};
//    mudnac::Tensor::Vector hi = {1, 64};

//    mudnac::Tensor::Vector lo = {0, 13};
//    mudnac::Tensor::Vector hi = {1, 77};

    mudnac::Tensor::Vector lo = {0, 0};
    mudnac::Tensor::Vector hi = {5, 10};

    uint64 bytes = tensor.getBurstQue(maxBurstBytes, lo, hi, bQue, spmTensor, spmAddrQue);
    printf("idealBytes=%lu, numBursts=%zu\n", bytes, bQue.size());
    for (int i = 0; i < bQue.size(); i++) {
        auto &burst = bQue[i];
        auto spmAddr = spmAddrQue[i];
        printf("(%lu,%lu,%lu) ", burst[0], burst[1], spmAddr);
    }
    printf("\n");
}
