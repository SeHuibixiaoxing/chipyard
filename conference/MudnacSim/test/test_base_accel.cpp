#include <cstdio>
#include "base_accel.h"


using namespace mudnac;


void test_conv_compute();

void test_conv_store();

void test_conv_store2();

void test_conv_load();

void test_conv_load2();


int main() {

    test_conv_compute();
    test_conv_store();
    test_conv_store2();
    test_conv_load();
    test_conv_load2();

    return 0;
}


void test_conv_compute() {
    AccelConfig cfg(0, 16, 16, 8, 64, 1);
    AccelTiledConv conv;

    conv.n = 1;
    conv.ic = 1;
    conv.oc = 1;
    conv.oh = 1;
    conv.ow = 1;
    conv.kh = 1;
    conv.kw = 1;
    conv.g = 1;
    conv.strideH = 1;
    conv.strideW = 1;
    conv.KH = 1;
    conv.KW = 1;

    printf("%lu\n", conv.compute(cfg));

    conv.ic = 16; conv.oc = 16;
    printf("%lu\n", conv.compute(cfg));

    conv.ic = 17; conv.oc = 17;
    printf("%lu\n", conv.compute(cfg));

    conv.ic = 3; conv.oc = 16; conv.g = 4;
    printf("%lu\n", conv.compute(cfg));

    conv.ic = 3; conv.oc = 8; conv.g = 4;
    printf("%lu\n", conv.compute(cfg));

    conv.ic = 3; conv.oc = 1; conv.g = 4;
    printf("%lu\n", conv.compute(cfg));

    conv.ic = 16; conv.oc = 16; conv.kh = 3; conv.kw = 3;
    printf("%lu\n", conv.compute(cfg));

    conv.kh = 5; conv.kw = 5;
    printf("%lu\n", conv.compute(cfg));

    conv.kh = 1; conv.kw = 1; conv.oh = 10; conv.ow = 10; conv.n = 10;
    printf("%lu\n", conv.compute(cfg));

    conv.kh = 3; conv.kw = 3;
    printf("%lu\n", conv.compute(cfg));
}


void test_conv_store() {
    AccelConfig cfg(0, 16, 16, 8, 64, 1);
    AccelTiledConv conv;
    conv.output.set(0, {100000, 10000, 1000, 100, 10});

//    conv.oh = 1; conv.ow = 1; conv.n = 1; conv.g = 1; conv.oc = 1;
//    conv.oh = 1; conv.ow = 1; conv.n = 1; conv.g = 1; conv.oc = 10;
//    conv.oh = 1; conv.ow = 1; conv.n = 1; conv.g = 10; conv.oc = 1;
//    conv.oh = 1; conv.ow = 1; conv.n = 10; conv.g = 1; conv.oc = 1;
//    conv.oh = 1; conv.ow = 10; conv.n = 1; conv.g = 1; conv.oc = 1;
//    conv.oh = 10; conv.ow = 1; conv.n = 1; conv.g = 1; conv.oc = 1;
    conv.oh = 2; conv.ow = 2; conv.n = 2; conv.g = 2; conv.oc = 2;

    Tensor::BurstQue bQue;
    uint64_t bytes = conv.store(0, cfg, bQue);
    printf("idealBytes=%lu, numBurst=%zu\n", bytes, bQue.size());
    for (auto a : bQue) {
        printf("(%lu,%lu) ", a[0], a[1]);
    }
    printf("\n");
}


void test_conv_store2() {
    AccelConfig cfg(0, 16, 16, 8, 64, 1);
    AccelTiledConv conv;
    conv.output.set(0, {100000, 10000, 1000, 100, 10});
    conv.spmOutput.tensor.set(8000000, {2560, 640, 160, 40, 10});

//    conv.oh = 1; conv.ow = 1; conv.n = 1; conv.g = 1; conv.oc = 1;
//    conv.oh = 1; conv.ow = 1; conv.n = 1; conv.g = 1; conv.oc = 4;
//    conv.oh = 1; conv.ow = 1; conv.n = 1; conv.g = 4; conv.oc = 1;
//    conv.oh = 1; conv.ow = 1; conv.n = 4; conv.g = 1; conv.oc = 1;
//    conv.oh = 1; conv.ow = 4; conv.n = 1; conv.g = 1; conv.oc = 1;
//    conv.oh = 4; conv.ow = 1; conv.n = 1; conv.g = 1; conv.oc = 1;
    conv.oh = 4; conv.ow = 4; conv.n = 4; conv.g = 4; conv.oc = 4;

    Tensor::BurstQue bQue;
    std::deque<uint64> spmAddrQue;
    uint64_t bytes = conv.store(0, cfg, bQue, spmAddrQue);
    printf("idealBytes=%lu, numBurst=%zu\n", bytes, bQue.size());
    for (int i = 0; i < bQue.size(); i++) {
        printf("(%lu,%lu,%lu) ", bQue[i][0], spmAddrQue[i], bQue[i][1]);
    }
    printf("\n");
}


void test_conv_load() {
    AccelConfig cfg(0, 16, 16, 8, 64, 1);
    AccelTiledConv c;
    c.input.set(0, {100000, 10000, 100, 10, 1});
//    c.input.set(0, {1, 1, 1, 1});

//    c.n = 1; c.ic = 5; c.oc = 5; c.oh = 1; c.ow = 1; c.kh = 3; c.kw = 3; c.g = 1;
//    c.strideH = 1; c.strideW = 1; c.KH = 3; c.KW = 3;

//    c.n = 1; c.ic = 5; c.oc = 5; c.oh = 1; c.ow = 1; c.kh = 3; c.kw = 3; c.g = 1;
//    c.strideH = 1; c.strideW = 1; c.KH = 10; c.KW = 10;

//    c.n = 1; c.ic = 5; c.oc = 5; c.oh = 1; c.ow = 1; c.kh = 3; c.kw = 3; c.g = 10;
//    c.strideH = 1; c.strideW = 1; c.KH = 3; c.KW = 3;

    c.oh = 4; c.ow = 4; c.n = 1; c.ic = 1; c.kh = 1; c.kw = 1; c.g = 1;
    c.strideH = 1; c.strideW = 1; c.KH = 1; c.KW = 1;

//    c.oh = 4; c.ow = 4; c.n = 1; c.ic = 1; c.kh = 3; c.kw = 3; c.g = 1;
//    c.strideH = 1; c.strideW = 1; c.KH = 3; c.KW = 3;

//    c.oh = 4; c.ow = 4; c.n = 1; c.ic = 1; c.kh = 3; c.kw = 3; c.g = 1;
//    c.strideH = 2; c.strideW = 2; c.KH = 3; c.KW = 3;

//    c.oh = 4; c.ow = 4; c.n = 1; c.ic = 1; c.kh = 1; c.kw = 1; c.g = 1;
//    c.strideH = 2; c.strideW = 2; c.KH = 1; c.KW = 1;

//    c.oh = 4; c.ow = 4; c.n = 1; c.ic = 1; c.kh = 3; c.kw = 3; c.g = 1;
//    c.strideH = 5; c.strideW = 5; c.KH = 3; c.KW = 3;

//    c.n = 1; c.ic = 1; c.oc = 1; c.oh = 1024; c.ow = 1; c.kh = 1; c.kw = 1; c.g = 1;
//    c.strideH = 1; c.strideW = 1; c.KH = 1; c.KW = 1;

    Tensor::BurstQue bQue;
    uint64_t bytes = c.load(2, cfg, bQue);
    printf("idealBytes=%lu, numBurst=%zu\n", bytes, bQue.size());
    for (auto a : bQue) {
        printf("(%lu,%lu) ", a[0], a[1]);
    }
    printf("\n");
}

void test_conv_load2() {
    AccelConfig cfg(0, 16, 16, 8, 64, 1);
    AccelTiledConv c;
    c.input.set(0, {100000, 10000, 100, 10, 1});
    c.spmInput.tensor.set(8000000, {32, 8, 4, 4, 1});  // TIH=4, TIW=4, TN=1, TG=2, TIC=4

    c.n = 1; c.ic = 4; c.oc = 4; c.oh = 2; c.ow = 2; c.kh = 3; c.kw = 3; c.g = 2;
    c.strideH = 1; c.strideW = 1; c.KH = 3, c.KW = 3;

    Tensor::BurstQue bQue;
    std::deque<uint64> spmAddrQue;
    uint64_t bytes = c.load(2, cfg, bQue, spmAddrQue);
    printf("idealBytes=%lu, numBurst=%zu\n", bytes, bQue.size());
    for (int i = 0; i < bQue.size(); i++) {
        printf("(%lu,%lu,%lu) ", bQue[i][0], spmAddrQue[i], bQue[i][1]);
    }
    printf("\n");
}
