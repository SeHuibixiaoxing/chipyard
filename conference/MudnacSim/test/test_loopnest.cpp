//#include "mapping.h"
#include "loopnest.h"


using namespace mudnac;


std::string to_string(const std::vector<uint> &vec);

void test_singleLevel();

void test_multiLevel();

void test_allIn();


int main() {
//    test_singleLevel();
//    test_multiLevel();
    test_allIn();

    return 0;
}


//void test_singleLevel() {
//    uint numLevels = 1;
////    uint numDims = 2;
//    uint numDims = 4;
//
////    std::vector<uint> tensorShape = {20, 10};
//    std::vector<uint> tensorShape = {10000, 1000, 100, 10};
////    std::vector<uint> tileShape = {5, 2};
////    std::vector<uint> tileShape = {6, 3};
//    std::vector<uint> tileShape = {2000, 300, 40, 6};
//
//    LoopNest ln;
//    ln.set(numLevels, numDims, tensorShape, tileShape);
//
////    ln.setLoop(0, 0, 0, 4);
////    ln.setLoop(0, 1, 1, 5);
////    ln.setLoop(0, 0, 1, 5);
////    ln.setLoop(0, 1, 0, 4);
////    ln.setLoop(0, 0, 0, 100);  // test invalid
////    ln.setLoop(0, 1, 1, 5);
////    ln.setLoop(0, 0, 0, 4);  // test invalid
////    ln.setLoop(0, 1, 1, 100);
////    ln.setLoop(0, 0, 0, 4);  // test tile shape
////    ln.setLoop(0, 1, 1, 4);
//
//    ln.setLoop(0, 0, 2, 10);  // test 4 dims
//    ln.setLoop(0, 1, 1, 10);
//    ln.setLoop(0, 2, 3, 10);
//    ln.setLoop(0, 3, 0, 10);
//
//    ln.clear();
//    while (ln.valid) {
//        printf("%u: dimIter=%s, curTileShape=%s\n",
//               ln.count, to_string(ln.dimIter).c_str(), to_string(ln.currentTileShape).c_str());
//        ln.next();
//    }
//    printf("%u\n", ln.count);
//}
//
//
//void test_multiLevel() {
//    uint numDims = 1;
////    uint numLevels = 2;
//    uint numLevels = 4;
//
////    std::vector<uint> tensorShape = {100};
//    std::vector<uint> tensorShape = {1000};
////    std::vector<uint> tileShape = {10};
//    std::vector<uint> tileShape = {15};
//
//    LoopNest ln;
//    ln.set(numLevels, numDims, tensorShape, tileShape);
//
////    ln.setLoop(0, 0, 0, 2);
////    ln.setLoop(1, 0, 0, 5);
////    ln.setLoop(0, 0, 0, 5);
////    ln.setLoop(1, 0, 0, 2);
////    ln.setLoop(0, 0, 0, 100);  // test invalid
////    ln.setLoop(1, 0, 0, 3);
////    ln.setLoop(0, 0, 0, 3);  // test invalid
////    ln.setLoop(1, 0, 0, 100);
////    ln.setLoop(0, 0, 0, 100);  // test tile shape
////    ln.setLoop(1, 0, 0, 3);
////    ln.setLoop(0, 0, 0, 100);  // test tile shape
////    ln.setLoop(1, 0, 0, 3);
//
//    ln.setLoop(0, 0, 0, 2);  // test 4 dims
//    ln.setLoop(1, 0, 0, 3);
//    ln.setLoop(2, 0, 0, 4);
//    ln.setLoop(3, 0, 0, 5);
//
//    ln.clear();
//    while (ln.valid) {
//        printf("%u: level=%u, dimIter=%s, curTileShape=%s\n",
//               ln.count, ln.level, to_string(ln.dimIter).c_str(), to_string(ln.currentTileShape).c_str());
//        ln.next();
//    }
//    printf("%u\n", ln.count);
//}
//
//
//void test_allIn() {
//    uint numDims = 2;
//    uint numLevels = 2;
//
//    std::vector<uint> tensorShape = {1000, 100};
//    std::vector<uint> tileShape = {128, 32};
//
//    LoopNest ln;
//    ln.set(numLevels, numDims, tensorShape, tileShape);
//
//    ln.setLoop(0, 0, 0, 2);
//    ln.setLoop(0, 1, 1, 10);
//    ln.setLoop(1, 0, 0, 5);
//    ln.setLoop(1, 1, 1, 4);
//
////    ln.setLoop(0, 0, 0, 2);
////    ln.setLoop(0, 1, 1, 10);
////    ln.setLoop(1, 0, 1, 4);
////    ln.setLoop(1, 1, 0, 5);
//
////    ln.setLoop(0, 0, 1, 10);
////    ln.setLoop(0, 1, 0, 2);
////    ln.setLoop(1, 0, 1, 4);
////    ln.setLoop(1, 1, 0, 5);
//
//    ln.clear();
//    while (ln.valid) {
//        printf("%u: level=%d, dimIter=%s, curTileShape=%s\n",
//               ln.count, ln.level, to_string(ln.dimIter).c_str(), to_string(ln.currentTileShape).c_str());
//        ln.next();
//    }
//    printf("%u\n", ln.count);
//}


void test_singleLevel() {
    LoopNest ln;

    std::cout << "ln.init\n";
//    ln.init({{8, 4, 2}}, {{0, 1, 2}}, {1000, 100, 10}));
//    ln.init({{8, 4, 2}}, {{2, 1, 0}}, {1000, 100, 10}));
    ln.init({{8, 4, 2}}, {{2, 0, 1}}, {1000, 100, 10});

    std::cout << "numLevels: " << ln.numLevels << "\n";
    std::cout << "numDims: " << ln.numDims << "\n";
    std::cout << "factors: " << toString(ln.factors) << "\n";
    std::cout << "loops: " << toString(ln.loops) << "\n";
    std::cout << "strides: " << toString(ln.strides) << "\n";
    std::cout << "iters: " << toString(ln.iters) << "\n";
    std::cout << "dimIters: " << toString(ln.dists) << "\n";

    while (ln.valid) {
        std::cout << ln.count << ":\n";
        std::cout << "iters: " << toString(ln.iters) << "\n";
        std::cout << "dimIters: " << toString(ln.dists) << "\n";
        ln.next();
    }
}


void test_multiLevel() {
    LoopNest ln;

    std::cout << "ln.init\n";
//    ln.init({{4}, {3}, {2}}, {{0}, {0}, {0}}, {10});
    ln.init({{2}, {3}, {4}}, {{0}, {0}, {0}}, {10});

    std::cout << "numLevels: " << ln.numLevels << "\n";
    std::cout << "numDims: " << ln.numDims << "\n";
    std::cout << "factors: " << toString(ln.factors) << "\n";
    std::cout << "loops: " << toString(ln.loops) << "\n";
    std::cout << "strides: " << toString(ln.strides) << "\n";
    std::cout << "iters: " << toString(ln.iters) << "\n";
    std::cout << "dimIters: " << toString(ln.dists) << "\n";

    while (ln.valid) {
        std::cout << ln.count << ":\n";
        std::cout << "iters: " << toString(ln.iters) << "\n";
        std::cout << "dimIters: " << toString(ln.dists) << "\n";

        std::vector<uint64> dists;
        ln.getRelativeDistance(dists, 0);
        printf("getRelativeDistance: targetLevelId=0, %s\n", toString(dists).c_str());
        ln.getRelativeDistance(dists, 1);
        printf("getRelativeDistance: targetLevelId=1, %s\n", toString(dists).c_str());
        ln.getRelativeDistance(dists, 2);
        printf("getRelativeDistance: targetLevelId=2, %s\n", toString(dists).c_str());

        ln.next();
    }
}


void test_allIn() {
    LoopNest ln;
    std::cout << "ln.init\n";
//    ln.init({{4, 5}, {2, 3}}, {{0, 1}, {0, 1}}, {100, 10});
    ln.init({{4, 5}, {2, 3}}, {{0, 1}, {1, 0}}, {100, 10});

    std::cout << "numLevels: " << ln.numLevels << "\n";
    std::cout << "numDims: " << ln.numDims << "\n";
    std::cout << "factors: " << toString(ln.factors) << "\n";
    std::cout << "loops: " << toString(ln.loops) << "\n";
    std::cout << "strides: " << toString(ln.strides) << "\n";
    std::cout << "iters: " << toString(ln.iters) << "\n";
    std::cout << "dimIters: " << toString(ln.dists) << "\n";

    while (ln.valid) {
        std::cout << ln.count << ":\n";
        std::cout << "iters: " << toString(ln.iters) << "\n";
        std::cout << "dimIters: " << toString(ln.dists) << "\n";

        std::vector<uint64> dists;
        ln.getRelativeDistance(dists, 0);
        printf("getRelativeDistance: targetLevelId=0, %s\n", toString(dists).c_str());
        ln.getRelativeDistance(dists, 1);
        printf("getRelativeDistance: targetLevelId=1, %s\n", toString(dists).c_str());

        ln.next();
    }
}


std::string to_string(const std::vector<uint> &vec) {
    std::string s;
    s += "[";
    for (auto a :vec) {
        s += std::to_string(a) + ",";
    }
    s += "]";
    return s;
}
