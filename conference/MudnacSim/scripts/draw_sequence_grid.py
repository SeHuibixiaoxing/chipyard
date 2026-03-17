#!/usr/bin/env python3
"""
draw_sequence_grid.py

Creates an n x m grid (coordinates start at (0,0) in the top-left; x increases to the right,
 y increases downward) and draws straight lines connecting a hard-coded sequence of grid
 points. Saves the output image `draw_sequence_grid.png` in the working directory.

Requires: matplotlib
Install: pip install matplotlib
Run: python3 draw_sequence_grid.py
"""

import matplotlib.pyplot as plt
from typing import List, Tuple

# --- Configuration (hard-coded) ---
# Grid width (n, number of columns) and height (m, number of rows)
n = 8  # columns (x: 0 .. n-1)
m = 16   # rows    (y: 0 .. m-1)

# Sequence of cell indices (row-major order) to connect. Indexing starts at top-left = 0
# numbering increases to the right, then continues on the next row. For an n x m grid:
# x = idx % n, y = idx // n
# Example sequence: indices that map to a path across the grid.
sequence_idx: List[int] = [0,1,9,8,16,24,25,17,18,26,27,19,11,10,2,3,4,12,13,5,6,7,15,14,22,23,31,30,29,21,20,28,36,44,45,37,38,39,47,46,54,55,63,62,61,53,52,60,59,58,50,51,43,35,34,42,41,33,32,40,48,49,57,56,64,72,73,65,66,67,75,74,82,83,91,90,89,81,80,88,96,97,105,104,112,120,121,113,114,122,123,115,107,106,98,99,100,101,109,108,116,124,125,117,118,126,127,119,111,110,102,103,95,87,86,94,93,92,84,85,77,76,68,69,70,78,79,71,]

# Output filename
OUTFILE = "draw_sequence_grid.png"

# --- Drawing utility ---

def validate_index_sequence(seq_idx: List[int], n: int, m: int) -> None:
    max_idx = n * m - 1
    for idx in seq_idx:
        if not (0 <= idx <= max_idx):
            raise ValueError(f"Index {idx} out of bounds 0..{max_idx}")


def draw_sequence_on_grid(n: int, m: int, seq_idx: List[int], outfile: str = OUTFILE, show: bool = True) -> None:
    validate_index_sequence(seq_idx, n, m)

    # Convert index sequence to (x,y) coordinate pairs (row-major)
    seq: List[Tuple[int, int]] = []
    for idx in seq_idx:
        x = idx % n
        y = idx // n
        seq.append((x, y))

    # Prepare figure size proportional to grid
    fig_w = max(6, n * 0.4)
    fig_h = max(4, m * 0.4)
    fig, ax = plt.subplots(figsize=(fig_w, fig_h))

    # Draw cell grid lines (drawn between integer coordinates centered on cells)
    for x in range(n + 1):
        ax.plot([x - 0.5, x - 0.5], [-0.5, m - 0.5], color="#dddddd", linewidth=1)
    for y in range(m + 1):
        ax.plot([-0.5, n - 0.5], [y - 0.5, y - 0.5], color="#dddddd", linewidth=1)

    # Plot the path: x and y arrays
    xs = [p[0] for p in seq]
    ys = [p[1] for p in seq]

    # Draw lines between consecutive points
    ax.plot(xs, ys, '-o', color='tab:blue', linewidth=2, markersize=8)

    # Annotate each point with its sequence index and coordinates
    for order, (x, y) in enumerate(seq):
        # show the original index and the sequence order
        orig_idx = seq_idx[order]
        ax.text(x, y - 0.18, str(order), color='black', fontsize=10, ha='center', va='bottom', weight='bold')
        ax.text(x, y + 0.12, f"{orig_idx}", color='gray', fontsize=8, ha='center', va='top')

    # Configure axes: show ticks at integer coordinates and invert y to have (0,0) top-left
    ax.set_xlim(-0.5, n - 0.5)
    ax.set_ylim(-0.5, m - 0.5)
    ax.set_xticks(list(range(n)))
    ax.set_yticks(list(range(m)))
    ax.set_aspect('equal')
    ax.invert_yaxis()  # y increases downward as requested (top-left is (0,0))

    # Labels and grid styling
    ax.set_xlabel('x')
    ax.set_ylabel('y')
    ax.set_title(f"Sequence on {n}×{m} grid, {len(seq)} points")
    ax.tick_params(axis='both', which='major', labelsize=9)

    # Save and optionally show
    plt.tight_layout()
    plt.savefig(outfile, dpi=200)
    print(f"Saved image to {outfile}")
    if show:
        plt.show()


if __name__ == '__main__':
    draw_sequence_on_grid(n, m, sequence_idx, OUTFILE, show=True)
