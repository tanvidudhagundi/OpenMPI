# Large-Scale Business Data Analysis

C++ system for analyzing the e-commerce dataset in `archive`, with sequential
and OpenMP-parallel implementations. It checks that both implementations
produce equivalent results and benchmarks execution time as the input prefix
and OpenMP thread count increase.

## Requirements

- GCC/MinGW with OpenMP support (`g++` and `-fopenmp` on `PATH`)
- The CSV at `archive\ecommerce_dataset_+1m.csv` (the default)

If the compiler is MinGW.org GCC with its `pthreadGC-3.dll` runtime but no
pthread import library, `run.bat` generates a local import library under
`build\openmp` as a linker compatibility workaround.

## Run on Windows

From PowerShell in the project directory:

```powershell
.\run.bat
```

Default benchmark: 10%, 25%, 50%, and 100% of the loaded rows, using 1, 2, 4,
and 8 OpenMP threads, with three repetitions per measurement. For a quick
smoke run:

```powershell
.\run.bat --limit 100000 --threads 1,2,4 --repeats 1
```

To choose another input or output directory:

```powershell
.\run.bat --input "archive\ecommerce_dataset_+1m.csv" --output results `
  --threads 1,2,4,8 --repeats 5
```

`--limit 0` means all rows. Results are written to:

- `results\summary.csv`: order count, revenue, profit, units sold, average
  order value, average rating, returned orders, and category/country/status
  breakdowns.
- `results\benchmark.csv`: row count, thread count, sequential and parallel
  median time, speedup, and parallel efficiency.
- `results\performance.svg`: a speedup-vs-input-size graph for each thread
  count.
- `results\performance_summary.txt`: best observed configuration and a concise
  measured conclusion, including a separate result for the largest input size.

## Design

The CSV loader streams each row and retains only revenue, profit, quantity,
rating, and integer identifiers for category, country, and order status. This
keeps the in-memory dataset substantially smaller than the source CSV. CSV
fields enclosed in quotes, commas inside quoted fields, and escaped quotes are
handled.

The sequential model processes the compact records in order. The OpenMP model
uses `#pragma omp parallel` and `#pragma omp for schedule(static)`: each worker
updates private counters and aggregation arrays, which are combined after the
parallel region. No shared counters are updated inside the loop.

Every parallel configuration is checked against the sequential result,
including revenue/profit totals and category, country, status, rating, quantity,
and returned-order counts. Floating-point sums use a small tolerance because
parallel reduction changes the order of addition.

## Performance analysis

- **Execution time:** median aggregation wall time; each timing sample repeats
  an appropriately sized batch to avoid timer-resolution artifacts. CSV loading
  is reported separately.
- **Speedup:** sequential time divided by parallel time.
- **Efficiency:** speedup divided by the number of requested threads.
- **Scalability:** the benchmark increases the input prefix size up to the full
  dataset and tests configurable OpenMP worker counts.

OpenMP runtime/thread setup and partial-result combination are included in
parallel timing, so smaller datasets may show overhead rather than a speedup.
