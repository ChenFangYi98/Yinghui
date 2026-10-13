#pragma once
#include <ppl.h>
namespace comp {
template <class Body> void parallelRows(int rows, Body body) {
    if (rows < 128) {
        for (int row = 0; row < rows; ++row)
            body(row);
    } else
        concurrency::parallel_for(0, rows, body);
}
} // namespace comp
