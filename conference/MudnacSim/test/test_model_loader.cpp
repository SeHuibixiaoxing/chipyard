#include "model_loader.h"

using namespace mudnac;


int main() {

    Model m;

    ModelLoader::load(m, "models/resnet50", 10000000, 10);

//    printf("%s\n", m.toString().c_str());
    printf("\n");

//    uint layerIdx = 0;
//    uint layerIdx = 3;
    uint layerIdx = 5;

//    printf("%s\n", m.layers[layerIdx]->mappingTable[0][0].toString().c_str());
//    printf("%s\n", m.layers[layerIdx]->mappingTable[0][1].toString().c_str());
    printf("%s\n", m.layers[layerIdx]->mappingTable[0][2].toString().c_str());
    printf("%s\n", m.layers[layerIdx]->mappingTable[0][7].toString().c_str());
//    printf("%s\n", m.layers[layerIdx]->mappingTable[1][0].toString().c_str());
//    printf("%s\n", m.layers[layerIdx]->mappingTable[2][0].toString().c_str());
    printf("\n");

    return 0;
}
