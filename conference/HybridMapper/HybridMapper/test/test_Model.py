from HybridMapper.Model import *


def test1():
    model = Model()

    # A residual block structure
    model.appendLayer(LayerConv([1, 64, 256, 56, 56, 1, 1, 1, 2, 2], [-1, -1, 0, 1],
                                dramKeep=[1, 1, 1, 0], additionalSpmDataList=[], layerGroupType=Layer.LG_TYPE_HEAD))
    model.appendLayer(LayerConv([1, 256, 256, 56, 56, 3, 3, 1, 1, 1], [-1, -1, 1, 2],
                                dramKeep=[1, 1, 0, 0], additionalSpmDataList=[0], layerGroupType=Layer.LG_TYPE_INTER))
    model.appendLayer(LayerConv([1, 256, 64, 56, 56, 1, 1, 1, 1, 1], [-1, -1, 2, 3],
                                dramKeep=[1, 1, 0, 0], additionalSpmDataList=[0], layerGroupType=Layer.LG_TYPE_INTER))
    model.appendLayer(LayerConv([1, 64, 64, 56, 56, 1, 1, 1, 2, 2], [-1, -1, 0, 4],
                                dramKeep=[1, 1, 0, 0], additionalSpmDataList=[3], layerGroupType=Layer.LG_TYPE_INTER))
    model.appendLayer(LayerResadd([1, 64, 56, 56, 1], [3, 4, 5],
                                  dramKeep=[0, 0, 1], additionalSpmDataList=[], layerGroupType=Layer.LG_TYPE_TAIL))

    model.generateMemoryMapping()

    print()
    print(model.getInfoString())
    print()
    print(model.getLayersString())
    print()

    probList = model.getProblemList()
    for i, prob in enumerate(probList):
        print(f"prob {i} : {str(prob)}")

    model.write("HybridMapper/test/test_Model_output/layers.yaml")


if __name__ == "__main__":
    test1()
