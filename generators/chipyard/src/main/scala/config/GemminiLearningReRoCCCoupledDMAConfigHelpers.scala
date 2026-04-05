package chipyard

object GemminiLearningReRoCCCoupledDMAConfigHelpers {
  case class NoCLayout(
    nocCols: Int,
    nocRows: Int,
    cpuNodes: Seq[Int],
    managerNodes: Seq[Int],
    infraNodes: Seq[Int]
  )

  def rowMajorNodes(count: Int, rowBase: Int, colsPerRow: Int, meshCols: Int): Seq[Int] = {
    (0 until count).map { i =>
      val row = rowBase + (i / colsPerRow)
      val col = i % colsPerRow
      row * meshCols + col
    }
  }

  def buildLayout(
    numCores: Int,
    cpuX: Int,
    cpuY: Int,
    numGemmini: Int,
    gemminiX: Int,
    gemminiY: Int,
    numDMA: Int,
    dmaX: Int,
    dmaY: Int,
    nMemoryChannels: Int,
    extraInfraEndpoints: Int,
    useCompactPairedManagerLayout: Boolean
  ): NoCLayout = {
    val infraEndpointCount = nMemoryChannels + extraInfraEndpoints
    val defaultNocCols = Seq(cpuX, gemminiX, dmaX, infraEndpointCount).max
    val defaultCpuNodes = rowMajorNodes(numCores, 0, cpuX, defaultNocCols)
    val defaultInfraRows = (infraEndpointCount + defaultNocCols - 1) / defaultNocCols
    val defaultCpuRowBase = 0
    val defaultGemminiRowBase = defaultCpuRowBase + cpuY
    val defaultDmaRowBase = defaultGemminiRowBase + gemminiY
    val defaultInfraRowBase = defaultDmaRowBase + dmaY
    val defaultNocRows = defaultInfraRowBase + defaultInfraRows

    val defaultGemminiNodes = rowMajorNodes(numGemmini, defaultGemminiRowBase, gemminiX, defaultNocCols)
    val defaultDmaNodes = rowMajorNodes(numDMA, defaultDmaRowBase, dmaX, defaultNocCols)
    val defaultInfraNodes = (0 until (defaultInfraRows * defaultNocCols)).map { i =>
      val row = defaultInfraRowBase + (i / defaultNocCols)
      val col = i % defaultNocCols
      row * defaultNocCols + col
    }

    if (useCompactPairedManagerLayout) {
      val compactNocCols = defaultNocCols
      val compactCpuNodes = rowMajorNodes(numCores, 0, cpuX, compactNocCols)
      val compactGemminiNodes = (0 until numGemmini).map { i =>
        val row = cpuY + 2 * (i / compactNocCols)
        val col = i % compactNocCols
        row * compactNocCols + col
      }
      val compactDmaNodes = (0 until numDMA).map { i =>
        val row = cpuY + 2 * (i / compactNocCols) + 1
        val col = i % compactNocCols
        row * compactNocCols + col
      }
      val managerRowPairs = (math.max(numGemmini, numDMA) + compactNocCols - 1) / compactNocCols
      val compactManagerEndRow = cpuY + managerRowPairs * 2
      val reservedTopNodes = (compactCpuNodes ++ compactGemminiNodes ++ compactDmaNodes).toSet
      val topFreeNodes = (0 until (cpuY * compactNocCols)).filterNot(reservedTopNodes.contains)
      val neededExtraInfra = math.max(infraEndpointCount - topFreeNodes.size, 0)
      val extraInfraRows = (neededExtraInfra + compactNocCols - 1) / compactNocCols
      val extraInfraNodes = (0 until (extraInfraRows * compactNocCols)).map { i =>
        val row = compactManagerEndRow + (i / compactNocCols)
        val col = i % compactNocCols
        row * compactNocCols + col
      }
      val compactInfraNodes = (topFreeNodes ++ extraInfraNodes).take(infraEndpointCount)
      val compactNocRows =
        (compactCpuNodes ++ compactGemminiNodes ++ compactDmaNodes ++ compactInfraNodes).max / compactNocCols + 1
      NoCLayout(
        nocCols = compactNocCols,
        nocRows = compactNocRows,
        cpuNodes = compactCpuNodes,
        managerNodes = compactGemminiNodes ++ compactDmaNodes,
        infraNodes = compactInfraNodes
      )
    } else {
      NoCLayout(
        nocCols = defaultNocCols,
        nocRows = defaultNocRows,
        cpuNodes = defaultCpuNodes,
        managerNodes = defaultGemminiNodes ++ defaultDmaNodes,
        infraNodes = defaultInfraNodes
      )
    }
  }

  def buildPairLayout(
    numCores: Int,
    cpuX: Int,
    cpuY: Int,
    numPairs: Int,
    pairX: Int,
    pairY: Int,
    nMemoryChannels: Int,
    extraInfraEndpoints: Int,
    useCompactPairManagerLayout: Boolean
  ): NoCLayout = {
    val infraEndpointCount = nMemoryChannels + extraInfraEndpoints
    val defaultNocCols = Seq(cpuX, pairX, infraEndpointCount).max
    val defaultCpuNodes = rowMajorNodes(numCores, 0, cpuX, defaultNocCols)
    val defaultPairRowBase = cpuY
    val defaultInfraRowBase = defaultPairRowBase + pairY
    val defaultInfraRows = (infraEndpointCount + defaultNocCols - 1) / defaultNocCols
    val defaultNocRows = defaultInfraRowBase + defaultInfraRows

    val defaultPairNodes = rowMajorNodes(numPairs, defaultPairRowBase, pairX, defaultNocCols)
    val defaultInfraNodes = (0 until (defaultInfraRows * defaultNocCols)).map { i =>
      val row = defaultInfraRowBase + (i / defaultNocCols)
      val col = i % defaultNocCols
      row * defaultNocCols + col
    }

    if (useCompactPairManagerLayout) {
      val compactNocCols = defaultNocCols
      val compactCpuNodes = rowMajorNodes(numCores, 0, cpuX, compactNocCols)
      val compactPairNodes = rowMajorNodes(numPairs, cpuY, compactNocCols, compactNocCols)
      val managerRows = (numPairs + compactNocCols - 1) / compactNocCols
      val compactManagerEndRow = cpuY + managerRows
      val reservedTopNodes = (compactCpuNodes ++ compactPairNodes).toSet
      val topFreeNodes = (0 until (cpuY * compactNocCols)).filterNot(reservedTopNodes.contains)
      val neededExtraInfra = math.max(infraEndpointCount - topFreeNodes.size, 0)
      val extraInfraRows = (neededExtraInfra + compactNocCols - 1) / compactNocCols
      val extraInfraNodes = (0 until (extraInfraRows * compactNocCols)).map { i =>
        val row = compactManagerEndRow + (i / compactNocCols)
        val col = i % compactNocCols
        row * compactNocCols + col
      }
      val compactInfraNodes = (topFreeNodes ++ extraInfraNodes).take(infraEndpointCount)
      val compactNocRows =
        (compactCpuNodes ++ compactPairNodes ++ compactInfraNodes).max / compactNocCols + 1
      NoCLayout(
        nocCols = compactNocCols,
        nocRows = compactNocRows,
        cpuNodes = compactCpuNodes,
        managerNodes = compactPairNodes,
        infraNodes = compactInfraNodes
      )
    } else {
      NoCLayout(
        nocCols = defaultNocCols,
        nocRows = defaultNocRows,
        cpuNodes = defaultCpuNodes,
        managerNodes = defaultPairNodes,
        infraNodes = defaultInfraNodes
      )
    }
  }
}
