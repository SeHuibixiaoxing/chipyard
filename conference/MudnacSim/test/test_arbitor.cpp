#include <iostream>
#include <tools.h>


using namespace mudnac;



void printAndCheck(bool ok, const char* s) {
    if(ok) printf("pass %s\n", s);
    else printf("%s failed\n", s);
}

bool testRRArbitor();
bool testRRArbitorWithQueue();

int main() {
    bool passTestRRArbitor = testRRArbitor();
    bool passTestRRArbitorWithQueue = testRRArbitorWithQueue();

    printf("\n\nSummary\n");
    printAndCheck(passTestRRArbitor, "testRRArbitor");
    printAndCheck(passTestRRArbitorWithQueue, "testRRArbitorWithQueue");

    return 0;
    
}


bool testRRArbitor() {    
    int groundtruth[16] = {0,2,3,4,0,2,3,4,3,4,3,4,-1,-1,1,4};
    int ans[16] = {0};
    int tot = 0;

    auto tickAndPrint = [&ans, &tot](Arbitor* arbitor) {
        arbitor->tick();
        printf("tick: %d\n", arbitor->getLastGrant());
        ans[tot ++] = arbitor->getLastGrant();
    };    

    RRArbitor arbitor(5);
    arbitor.enable(0);

    arbitor.enable(2);
    arbitor.enable(3);
    arbitor.enable(4);

    tickAndPrint(&arbitor); // 0
    tickAndPrint(&arbitor); // 2
    tickAndPrint(&arbitor); // 3
    tickAndPrint(&arbitor); // 4
    tickAndPrint(&arbitor); // 0
    tickAndPrint(&arbitor); // 2
    tickAndPrint(&arbitor); // 3
    tickAndPrint(&arbitor); // 4

    arbitor.disable(0);
    arbitor.disable(2);
    
    tickAndPrint(&arbitor); // 3
    tickAndPrint(&arbitor); // 4
    tickAndPrint(&arbitor); // 3
    tickAndPrint(&arbitor); // 4

    arbitor.disable(3);
    arbitor.disable(4);

    
    tickAndPrint(&arbitor); // -1
    tickAndPrint(&arbitor); // -1

    arbitor.enable(1);
    arbitor.enable(4);
    
    tickAndPrint(&arbitor); // 1
    tickAndPrint(&arbitor); // 4

    bool pass=true;
    for(int i = 0;i < 16;i ++) {
        if(ans[i] != groundtruth[i]) {
            printf("ans[%d] = %d, groundtruth[%d] = %d\n", i, ans[i], i, groundtruth[i]);
            pass = false;
        }
    }
    printf("pass testRRArbitor\n\n");

    return pass;
}


bool testRRArbitorWithQueue() {
    int groundtruth[25][2] = {
        {0, 10},
        {2, 20},
        {3, 30},
        {4, 40},
        {0, 100},
        {2, 200},
        {4, 400},        

        {0, 0},
        {1, 10},
        {2, 20},
        {3, 30},
        {4, 40},
        {0, 0},
        {1, 10},
        {2, 20},
        {3, 30},
        {4, 40},
        
        {1, 10},
        {2, 20},
        {3, 30},
        {4, 40},

        {2, 20},
        {3, 30},

        
        {3, 30},
        {-1, 0},
    };
    int ansElem[25] = {0};
    int ansIdx[25] = {0};
    int tot = 0;

    QueueWithArbitor<int> arbitorQueue(QueueWithArbitor<int>::ArbitorType::RRArbitor, 5);

    arbitorQueue.push(0, 10);
    arbitorQueue.push(2, 20);
    arbitorQueue.push(3, 30);
    arbitorQueue.push(4, 40);

    
    arbitorQueue.push(0, 100);
    arbitorQueue.push(2, 200);
    arbitorQueue.push(4, 400);



    auto tickAndPrint = [&]() {
        arbitorQueue.tick();
        std::tie(ansElem[tot], ansIdx[tot]) = arbitorQueue.pop();
        printf("tick idx %d elem %d\n", ansIdx[tot], ansElem[tot]);
        ++ tot;

        // Repeated consumption
        auto [t_elem, t_idx] = arbitorQueue.pop();
        if(!(t_idx == -1 && t_elem == 0)) {
            printf("repeated consumption: failed\n");
            assert(false);
        } else {
            printf("repeated consumption: ok\n");
        }
    };

    for(int i = 0;i < 7;++ i) {
        tickAndPrint();        
        // 0, 100
        // 2, 200
        // 3, 20
        // 4, 400
        // 0, 10
        // 2, 20
        // 4, 40
    }

    
    arbitorQueue.push(0, 0); //2
    arbitorQueue.push(1, 10); // 3
    arbitorQueue.push(2, 20); // 4
    arbitorQueue.push(3, 30); // 5
    arbitorQueue.push(4, 40); // 3

    arbitorQueue.push(0, 0);
    arbitorQueue.push(1, 10);
    arbitorQueue.push(2, 20);
    arbitorQueue.push(3, 30);
    arbitorQueue.push(4, 40);


    arbitorQueue.push(1, 10);
    arbitorQueue.push(2, 20);
    arbitorQueue.push(3, 30);
    arbitorQueue.push(4, 40);
    
    arbitorQueue.push(2, 20);
    arbitorQueue.push(3, 30);

    arbitorQueue.push(3, 30);

    for(int i = 0;i < 18;++ i) {
        tickAndPrint(); 
    }
    

    bool pass=true;
    for(int i = 0;i < 25;++ i) {
        if(ansElem[i] != groundtruth[i][1] || ansIdx[i] != groundtruth[i][0]) {
            printf("ans[%d] = {%d, %d}, groundtruth[%d] = {%d, %d}\n", i, ansIdx[i], ansElem[i], i, groundtruth[i][0], groundtruth[i][1]);
            pass = false;
        }
    }

    return pass;
}