#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>
#include <omp.h>
#include <sys/stat.h>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#endif

namespace {

const int kSelectedFieldCount = 7;
const char* const kDefaultInput = "archive\\ecommerce_dataset_+1m.csv";

struct Order {
    double revenue;
    double profit;
    long long quantity;
    double rating;
    int categoryId;
    int countryId;
    int statusId;
    bool returned;
};

struct Dataset {
    std::vector<Order> orders;
    std::vector<std::string> categories;
    std::vector<std::string> countries;
    std::vector<std::string> statuses;
    double loadSeconds;
};

struct Stats {
    long long rows;
    long long quantity;
    long long ratingCount;
    long long returnedOrders;
    double revenue;
    double profit;
    double ratingSum;
    std::vector<double> categoryRevenue;
    std::vector<long long> categoryCounts;
    std::vector<long long> countryCounts;
    std::vector<long long> statusCounts;
    double seconds;

    Stats(std::size_t categories, std::size_t countries, std::size_t statuses)
        : rows(0), quantity(0), ratingCount(0), returnedOrders(0),
          revenue(0.0), profit(0.0), ratingSum(0.0),
          categoryRevenue(categories, 0.0), categoryCounts(categories, 0),
          countryCounts(countries, 0), statusCounts(statuses, 0), seconds(0.0) {
    }

    void add(const Order& order) {
        ++rows;
        revenue += order.revenue;
        profit += order.profit;
        quantity += order.quantity;
        if (!std::isnan(order.rating)) {
            ratingSum += order.rating;
            ++ratingCount;
        }
        categoryRevenue[order.categoryId] += order.revenue;
        ++categoryCounts[order.categoryId];
        ++countryCounts[order.countryId];
        ++statusCounts[order.statusId];
        if (order.returned) {
            ++returnedOrders;
        }
    }

    void merge(const Stats& other) {
        rows += other.rows;
        quantity += other.quantity;
        ratingCount += other.ratingCount;
        returnedOrders += other.returnedOrders;
        revenue += other.revenue;
        profit += other.profit;
        ratingSum += other.ratingSum;
        for (std::size_t i = 0; i < categoryRevenue.size(); ++i) {
            categoryRevenue[i] += other.categoryRevenue[i];
            categoryCounts[i] += other.categoryCounts[i];
        }
        for (std::size_t i = 0; i < countryCounts.size(); ++i) {
            countryCounts[i] += other.countryCounts[i];
        }
        for (std::size_t i = 0; i < statusCounts.size(); ++i) {
            statusCounts[i] += other.statusCounts[i];
        }
    }
};

struct Options {
    std::string input;
    std::string outputDirectory;
    long long limit;
    int repeats;
    std::vector<int> threads;

    Options() : input(kDefaultInput), outputDirectory("results"), limit(0), repeats(3) {
        threads.push_back(1);
        threads.push_back(2);
        threads.push_back(4);
        threads.push_back(8);
    }
};

struct BenchmarkResult {
    std::size_t rows;
    int threads;
    double sequentialSeconds;
    double parallelSeconds;
    double speedup;
    double efficiency;
};

long long clockNanoseconds() {
#ifdef _WIN32
    static const double nanosecondsPerTick = []() {
        LARGE_INTEGER frequency;
        if (!QueryPerformanceFrequency(&frequency)) {
            throw std::runtime_error("High-resolution performance timer is unavailable.");
        }
        return 1e9 / static_cast<double>(frequency.QuadPart);
    }();
    LARGE_INTEGER counter;
    if (!QueryPerformanceCounter(&counter)) {
        throw std::runtime_error("Unable to read the high-resolution performance timer.");
    }
    return static_cast<long long>(counter.QuadPart * nanosecondsPerTick);
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

std::string trim(const std::string& value) {
    const std::string whitespace(" \t\r\n");
    const std::size_t first = value.find_first_not_of(whitespace);
    if (first == std::string::npos) {
        return "";
    }
    const std::size_t last = value.find_last_not_of(whitespace);
    return value.substr(first, last - first + 1);
}

std::string decodeField(const std::string& line, std::size_t start, std::size_t end) {
    std::string value = line.substr(start, end - start);
    if (value.size() >= 2 && value[0] == '"' && value[value.size() - 1] == '"') {
        value = value.substr(1, value.size() - 2);
        std::string decoded;
        decoded.reserve(value.size());
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '"' && i + 1 < value.size() && value[i + 1] == '"') {
                ++i;
            }
            decoded.push_back(value[i]);
        }
        value.swap(decoded);
    }
    return trim(value);
}

std::vector<std::string> parseCsvRecord(const std::string& line) {
    std::vector<std::string> fields;
    bool quoted = false;
    std::string field;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                field.push_back('"');
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if (c == ',' && !quoted) {
            fields.push_back(field);
            field.clear();
        } else {
            field.push_back(c);
        }
    }
    if (quoted) {
        throw std::runtime_error("Malformed CSV header: unmatched quote.");
    }
    fields.push_back(field);
    return fields;
}

std::vector<std::string> selectCsvFields(
        const std::string& line, std::size_t expectedColumns,
        const std::vector<std::size_t>& selectedColumns) {
    std::vector<std::string> selected(kSelectedFieldCount);
    bool quoted = false;
    std::size_t start = 0;
    std::size_t fieldIndex = 0;
    for (std::size_t i = 0; i <= line.size(); ++i) {
        if (i < line.size() && line[i] == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') {
                ++i;
            } else {
                quoted = !quoted;
            }
        } else if ((i == line.size() || line[i] == ',') && !quoted) {
            if (fieldIndex < expectedColumns) {
                for (int selectedIndex = 0; selectedIndex < kSelectedFieldCount; ++selectedIndex) {
                    if (selectedColumns[selectedIndex] == fieldIndex) {
                        selected[selectedIndex] = decodeField(line, start, i);
                        break;
                    }
                }
            }
            ++fieldIndex;
            start = i + 1;
        }
    }
    if (quoted || fieldIndex != expectedColumns) {
        throw std::runtime_error("Malformed CSV record: expected "
                + std::to_string(expectedColumns) + " columns, found "
                + std::to_string(fieldIndex) + ".");
    }
    return selected;
}

std::size_t requiredColumn(
        const std::unordered_map<std::string, std::size_t>& columns,
        const std::string& name) {
    std::unordered_map<std::string, std::size_t>::const_iterator found = columns.find(name);
    if (found == columns.end()) {
        throw std::runtime_error("Required CSV column is missing: " + name);
    }
    return found->second;
}

double parseDouble(const std::string& value, const std::string& column) {
    if (value.empty()) {
        return std::numeric_limits<double>::quiet_NaN();
    }
    char* end = NULL;
    const double parsed = std::strtod(value.c_str(), &end);
    if (end == value.c_str() || *end != '\0') {
        throw std::runtime_error("Invalid number in column " + column + ": " + value);
    }
    return parsed;
}

long long parseLong(const std::string& value, const std::string& column) {
    if (value.empty()) {
        return 0;
    }
    char* end = NULL;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0') {
        throw std::runtime_error("Invalid integer in column " + column + ": " + value);
    }
    return parsed;
}

int getId(std::unordered_map<std::string, int>& ids,
          std::vector<std::string>& names, const std::string& value) {
    const std::string normalized = value.empty() ? "(unknown)" : value;
    std::unordered_map<std::string, int>::const_iterator found = ids.find(normalized);
    if (found != ids.end()) {
        return found->second;
    }
    const int id = static_cast<int>(names.size());
    ids[normalized] = id;
    names.push_back(normalized);
    return id;
}

Dataset loadDataset(const std::string& path, long long limit) {
    const long long started = clockNanoseconds();
    std::ifstream input(path.c_str(), std::ios::in | std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open input CSV: " + path);
    }

    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("CSV is empty: " + path);
    }
    std::vector<std::string> headers = parseCsvRecord(line);
    if (!headers.empty() && headers[0].size() >= 3
            && static_cast<unsigned char>(headers[0][0]) == 0xEF
            && static_cast<unsigned char>(headers[0][1]) == 0xBB
            && static_cast<unsigned char>(headers[0][2]) == 0xBF) {
        headers[0] = headers[0].substr(3);
    }
    std::unordered_map<std::string, std::size_t> columns;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        columns[trim(headers[i])] = i;
    }
    const std::vector<std::string> names = {
        "total_price_usd", "profit_usd", "quantity", "rating",
        "category", "country", "order_status"
    };
    std::vector<std::size_t> selectedColumns;
    for (std::size_t i = 0; i < names.size(); ++i) {
        selectedColumns.push_back(requiredColumn(columns, names[i]));
    }

    std::unordered_map<std::string, int> categoryIds;
    std::unordered_map<std::string, int> countryIds;
    std::unordered_map<std::string, int> statusIds;
    Dataset dataset;
    while ((limit <= 0 || static_cast<long long>(dataset.orders.size()) < limit)
            && std::getline(input, line)) {
        const std::vector<std::string> field =
                selectCsvFields(line, headers.size(), selectedColumns);
        Order order;
        order.revenue = parseDouble(field[0], "total_price_usd");
        order.profit = parseDouble(field[1], "profit_usd");
        order.quantity = parseLong(field[2], "quantity");
        order.rating = parseDouble(field[3], "rating");
        order.categoryId = getId(categoryIds, dataset.categories, field[4]);
        order.countryId = getId(countryIds, dataset.countries, field[5]);
        order.statusId = getId(statusIds, dataset.statuses, field[6]);
        order.returned = field[6] == "Returned";
        dataset.orders.push_back(order);
    }
    dataset.loadSeconds = (clockNanoseconds() - started) / 1e9;
    return dataset;
}

Stats aggregateSequential(const Dataset& dataset, std::size_t size) {
    const long long started = clockNanoseconds();
    Stats stats(dataset.categories.size(), dataset.countries.size(), dataset.statuses.size());
    for (std::size_t i = 0; i < size; ++i) {
        stats.add(dataset.orders[i]);
    }
    stats.seconds = (clockNanoseconds() - started) / 1e9;
    return stats;
}

Stats aggregateParallel(const Dataset& dataset, std::size_t size, int threads) {
    const long long started = clockNanoseconds();
    std::vector<Stats> partials;
    for (int i = 0; i < threads; ++i) {
        partials.push_back(Stats(
                dataset.categories.size(), dataset.countries.size(), dataset.statuses.size()));
    }
#pragma omp parallel num_threads(threads)
    {
        const int threadId = omp_get_thread_num();
        Stats& local = partials[threadId];
#pragma omp for schedule(static)
        for (long long i = 0; i < static_cast<long long>(size); ++i) {
            local.add(dataset.orders[static_cast<std::size_t>(i)]);
        }
    }
    Stats combined(dataset.categories.size(), dataset.countries.size(), dataset.statuses.size());
    for (int i = 0; i < threads; ++i) {
        combined.merge(partials[i]);
    }
    combined.seconds = (clockNanoseconds() - started) / 1e9;
    return combined;
}

bool closeEnough(double left, double right) {
    if (std::isnan(left) || std::isnan(right)) {
        return std::isnan(left) && std::isnan(right);
    }
    return std::fabs(left - right) <= std::max(1e-7, std::fabs(left) * 1e-10);
}

bool closeArrays(const std::vector<double>& left, const std::vector<double>& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t i = 0; i < left.size(); ++i) {
        if (!closeEnough(left[i], right[i])) {
            return false;
        }
    }
    return true;
}

void verifyEquivalent(const Stats& expected, const Stats& actual, int threads) {
    if (expected.rows != actual.rows || expected.quantity != actual.quantity
            || expected.returnedOrders != actual.returnedOrders
            || expected.ratingCount != actual.ratingCount
            || !closeEnough(expected.revenue, actual.revenue)
            || !closeEnough(expected.profit, actual.profit)
            || !closeEnough(expected.ratingSum, actual.ratingSum)
            || expected.categoryCounts != actual.categoryCounts
            || expected.countryCounts != actual.countryCounts
            || expected.statusCounts != actual.statusCounts
            || !closeArrays(expected.categoryRevenue, actual.categoryRevenue)) {
        throw std::runtime_error("Sequential/parallel results differ at "
                + std::to_string(threads) + " threads.");
    }
}

long long medianSequential(const Dataset& dataset, std::size_t size, int repeats) {
    const std::size_t batchCount = std::max<std::size_t>(
            1, std::min<std::size_t>(500, 500000 / size));
    std::vector<long long> times;
    for (int i = 0; i < repeats; ++i) {
        const long long started = clockNanoseconds();
        volatile double checksum = 0.0;
        for (std::size_t batch = 0; batch < batchCount; ++batch) {
            checksum += aggregateSequential(dataset, size).revenue;
        }
        const long long elapsed = clockNanoseconds() - started;
        times.push_back(elapsed / static_cast<long long>(batchCount));
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

long long medianParallel(const Dataset& dataset, std::size_t size, int threads, int repeats) {
    const std::size_t batchCount = std::max<std::size_t>(
            1, std::min<std::size_t>(500, 500000 / size));
    std::vector<long long> times;
    for (int i = 0; i < repeats; ++i) {
        const long long started = clockNanoseconds();
        volatile double checksum = 0.0;
        for (std::size_t batch = 0; batch < batchCount; ++batch) {
            checksum += aggregateParallel(dataset, size, threads).revenue;
        }
        const long long elapsed = clockNanoseconds() - started;
        times.push_back(elapsed / static_cast<long long>(batchCount));
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

std::vector<std::size_t> benchmarkSizes(std::size_t rows) {
    const int percentages[] = {10, 25, 50, 100};
    std::vector<std::size_t> sizes;
    for (std::size_t i = 0; i < sizeof(percentages) / sizeof(percentages[0]); ++i) {
        const std::size_t size = std::max<std::size_t>(
                1, rows * static_cast<std::size_t>(percentages[i]) / 100);
        if (sizes.empty() || sizes.back() != size) {
            sizes.push_back(size);
        }
    }
    return sizes;
}

void writePerformanceArtifacts(const std::string& svgPath, const std::string& reportPath,
                              const std::vector<BenchmarkResult>& results,
                              const std::vector<int>& threadCounts) {
    const int width = 960;
    const int height = 560;
    const int left = 90;
    const int right = 710;
    const int top = 80;
    const int bottom = 430;
    double maxSpeedup = 1.0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        maxSpeedup = std::max(maxSpeedup, results[i].speedup);
    }
    maxSpeedup *= 1.1;
    const char* colors[] = {
        "#315f9b", "#e07a25", "#438b58", "#ad4b4b",
        "#875aa0", "#4b9a9a", "#a18a2c", "#555555"
    };

    std::ofstream svg(svgPath.c_str());
    if (!svg) {
        throw std::runtime_error("Cannot create performance graph: " + svgPath);
    }
    svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" << width
        << "\" height=\"" << height << "\" viewBox=\"0 0 "
        << width << " " << height << "\">\n"
        << "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n"
        << "<text x=\"480\" y=\"35\" text-anchor=\"middle\" font-family=\"Arial\""
        << " font-size=\"22\">Parallel speedup by input size</text>\n";
    for (int tick = 0; tick <= 5; ++tick) {
        const double value = maxSpeedup * tick / 5.0;
        const double y = bottom - (bottom - top) * tick / 5.0;
        svg << "<line x1=\"" << left << "\" y1=\"" << y << "\" x2=\""
            << right << "\" y2=\"" << y << "\" stroke=\"#dddddd\"/>\n"
            << "<text x=\"" << left - 12 << "\" y=\"" << y + 5
            << "\" text-anchor=\"end\" font-family=\"Arial\" font-size=\"12\">"
            << std::fixed << std::setprecision(1) << value << "</text>\n";
    }
    if (maxSpeedup > 1.0) {
        const double baselineY = bottom - (bottom - top) / maxSpeedup;
        svg << "<line x1=\"" << left << "\" y1=\"" << baselineY << "\" x2=\""
            << right << "\" y2=\"" << baselineY
            << "\" stroke=\"#555555\" stroke-dasharray=\"6,5\"/>\n";
    }
    svg << "<line x1=\"" << left << "\" y1=\"" << top << "\" x2=\""
        << left << "\" y2=\"" << bottom << "\" stroke=\"#222222\"/>\n"
        << "<line x1=\"" << left << "\" y1=\"" << bottom << "\" x2=\""
        << right << "\" y2=\"" << bottom << "\" stroke=\"#222222\"/>\n"
        << "<text x=\"28\" y=\"255\" transform=\"rotate(-90 28 255)\""
        << " text-anchor=\"middle\" font-family=\"Arial\" font-size=\"14\">Speedup</text>\n"
        << "<text x=\"400\" y=\"490\" text-anchor=\"middle\" font-family=\"Arial\""
        << " font-size=\"14\">Input rows</text>\n";

    std::vector<std::size_t> sizes;
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (sizes.empty() || sizes.back() != results[i].rows) {
            sizes.push_back(results[i].rows);
        }
    }
    for (std::size_t i = 0; i < sizes.size(); ++i) {
        const double x = sizes.size() == 1 ? (left + right) / 2.0
                : left + (right - left) * i / static_cast<double>(sizes.size() - 1);
        svg << "<text x=\"" << x << "\" y=\"" << bottom + 22
            << "\" text-anchor=\"middle\" font-family=\"Arial\" font-size=\"12\">"
            << sizes[i] << "</text>\n";
    }
    for (std::size_t threadIndex = 0; threadIndex < threadCounts.size(); ++threadIndex) {
        const char* color = colors[threadIndex % (sizeof(colors) / sizeof(colors[0]))];
        svg << "<polyline fill=\"none\" stroke=\"" << color
            << "\" stroke-width=\"2.5\" points=\"";
        for (std::size_t sizeIndex = 0; sizeIndex < sizes.size(); ++sizeIndex) {
            for (std::size_t resultIndex = 0; resultIndex < results.size(); ++resultIndex) {
                const BenchmarkResult& result = results[resultIndex];
                if (result.threads == threadCounts[threadIndex]
                        && result.rows == sizes[sizeIndex]) {
                    const double x = sizes.size() == 1 ? (left + right) / 2.0
                            : left + (right - left) * sizeIndex
                                    / static_cast<double>(sizes.size() - 1);
                    const double y = bottom - (bottom - top) * result.speedup / maxSpeedup;
                    svg << x << "," << y << " ";
                    break;
                }
            }
        }
        svg << "\"/>\n<text x=\"750\" y=\"" << 105 + threadIndex * 26
            << "\" font-family=\"Arial\" font-size=\"14\" fill=\"" << color
            << "\">" << threadCounts[threadIndex] << " thread(s)</text>\n";
        svg << "<line x1=\"720\" y1=\"" << 100 + threadIndex * 26
            << "\" x2=\"740\" y2=\"" << 100 + threadIndex * 26
            << "\" stroke=\"" << color << "\" stroke-width=\"3\"/>\n";
    }
    svg << "<text x=\"750\" y=\"245\" font-family=\"Arial\" font-size=\"12\""
        << " fill=\"#555555\">Dashed line: 1x (sequential time)</text>\n"
        << "</svg>\n";
    if (!svg) {
        throw std::runtime_error("Failed while writing performance graph: " + svgPath);
    }

    std::ofstream report(reportPath.c_str());
    if (!report) {
        throw std::runtime_error("Cannot create performance report: " + reportPath);
    }
    const BenchmarkResult* best = &results[0];
    std::size_t largestInput = results[0].rows;
    for (std::size_t i = 0; i < results.size(); ++i) {
        largestInput = std::max(largestInput, results[i].rows);
    }
    const BenchmarkResult* bestAtLargestInput = NULL;
    for (std::size_t i = 1; i < results.size(); ++i) {
        if (results[i].speedup > best->speedup) {
            best = &results[i];
        }
    }
    for (std::size_t i = 0; i < results.size(); ++i) {
        if (results[i].rows == largestInput
                && (bestAtLargestInput == NULL
                    || results[i].speedup > bestAtLargestInput->speedup)) {
            bestAtLargestInput = &results[i];
        }
    }
    report << std::fixed << std::setprecision(3)
           << "Best measured speedup: " << best->speedup << "x\n"
           << "Configuration: " << best->rows << " rows, " << best->threads
           << " OpenMP threads.\n"
           << "Sequential time: " << best->sequentialSeconds << " s\n"
           << "Parallel time: " << best->parallelSeconds << " s\n"
           << "Parallel efficiency: " << best->efficiency * 100.0 << "%\n\n"
           << "Largest-input result (" << largestInput << " rows): "
           << bestAtLargestInput->speedup << "x speedup with "
           << bestAtLargestInput->threads << " OpenMP threads.\n";
    if (bestAtLargestInput->speedup <= 1.0) {
        report << "Conclusion: no tested parallel configuration was faster than "
                  "the sequential implementation on the largest input; OpenMP "
                  "overhead and/or available CPU resources limited scaling here.\n";
    } else {
        report << "Conclusion: the fastest tested parallel configuration beat "
                  "the sequential implementation on the largest input. Compare "
                  "other points in performance.svg to assess size and thread scaling.\n";
    }
    report << "The overall best result may be a smaller input and should not be "
              "treated as full-dataset scaling evidence.\n";
    report << "Benchmark timings cover aggregation and OpenMP setup/combination; "
              "CSV loading is reported separately.\n";
}

std::string csvEscape(const std::string& value) {
    if (value.find_first_of(",\"\n") == std::string::npos) {
        return value;
    }
    std::string escaped = "\"";
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '"') {
            escaped.push_back('"');
        }
        escaped.push_back(value[i]);
    }
    escaped.push_back('"');
    return escaped;
}

void writeMetric(std::ofstream& output, const std::string& name, const std::string& value) {
    output << csvEscape(name) << "," << csvEscape(value) << "\n";
}

std::string numberString(double value) {
    std::ostringstream formatted;
    formatted << std::setprecision(15) << value;
    return formatted.str();
}

void writeCountRows(std::ofstream& output, const std::vector<std::string>& names,
                    const std::vector<long long>& counts) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        output << csvEscape(names[i]) << "," << counts[i] << "\n";
    }
}

void writeSummary(const std::string& path, const Stats& stats, const Dataset& dataset) {
    std::ofstream output(path.c_str());
    if (!output) {
        throw std::runtime_error("Cannot create summary output: " + path);
    }
    output << std::setprecision(15);
    output << "metric,value\n";
    writeMetric(output, "orders", std::to_string(stats.rows));
    writeMetric(output, "revenue_usd", numberString(stats.revenue));
    writeMetric(output, "profit_usd", numberString(stats.profit));
    writeMetric(output, "units_sold", std::to_string(stats.quantity));
    writeMetric(output, "average_order_value_usd",
            stats.rows == 0 ? "0" : numberString(stats.revenue / stats.rows));
    writeMetric(output, "average_rating",
            stats.ratingCount == 0 ? "N/A" : numberString(stats.ratingSum / stats.ratingCount));
    writeMetric(output, "returned_orders", std::to_string(stats.returnedOrders));
    writeMetric(output, "sequential_aggregation_seconds", numberString(stats.seconds));

    std::vector<int> order(dataset.categories.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = static_cast<int>(i);
    }
    std::sort(order.begin(), order.end(), [&stats](int left, int right) {
        return stats.categoryRevenue[left] > stats.categoryRevenue[right];
    });
    output << "\ncategory,orders,revenue_usd\n";
    for (std::size_t i = 0; i < order.size(); ++i) {
        const int id = order[i];
        output << csvEscape(dataset.categories[id]) << ","
               << stats.categoryCounts[id] << ","
               << stats.categoryRevenue[id] << "\n";
    }
    output << "\ncountry,orders\n";
    writeCountRows(output, dataset.countries, stats.countryCounts);
    output << "\norder_status,orders\n";
    writeCountRows(output, dataset.statuses, stats.statusCounts);
}

std::vector<int> parseThreads(const std::string& value) {
    std::vector<int> threads;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string token = trim(value.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start));
        errno = 0;
        char* end = NULL;
        const long parsed = std::strtol(token.c_str(), &end, 10);
        if (token.empty() || end == token.c_str() || *end != '\0'
                || errno == ERANGE || parsed < 1 || parsed > 256) {
            throw std::runtime_error(
                    "Thread counts must be integers between 1 and 256.");
        }
        threads.push_back(static_cast<int>(parsed));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return threads;
}

bool ensureOutputDirectory(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) == 0) {
        return (info.st_mode & S_IFDIR) != 0;
    }
#ifdef _WIN32
    return _mkdir(path.c_str()) == 0;
#else
    return mkdir(path.c_str(), 0755) == 0 || errno == EEXIST;
#endif
}

std::string requireValue(int argc, char** argv, int& index, const std::string& option) {
    if (index + 1 >= argc) {
        throw std::runtime_error("Missing value for " + option);
    }
    ++index;
    return argv[index];
}

long long parseIntegerOption(const std::string& value, const std::string& option) {
    errno = 0;
    char* end = NULL;
    const long long parsed = std::strtoll(value.c_str(), &end, 10);
    if (value.empty() || end == value.c_str() || *end != '\0' || errno == ERANGE) {
        throw std::runtime_error("Invalid integer for " + option + ": " + value);
    }
    return parsed;
}

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument == "--input") {
            options.input = requireValue(argc, argv, i, argument);
        } else if (argument == "--output") {
            options.outputDirectory = requireValue(argc, argv, i, argument);
        } else if (argument == "--limit") {
            options.limit = parseIntegerOption(
                    requireValue(argc, argv, i, argument), argument);
            if (options.limit < 0) {
                throw std::runtime_error("--limit must be zero or greater.");
            }
        } else if (argument == "--repeats") {
            const long long repeats = parseIntegerOption(
                    requireValue(argc, argv, i, argument), argument);
            if (repeats < 1 || repeats > 10000) {
                throw std::runtime_error("--repeats must be between 1 and 10000.");
            }
            options.repeats = static_cast<int>(repeats);
        } else if (argument == "--threads") {
            options.threads = parseThreads(requireValue(argc, argv, i, argument));
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "Usage: run.bat [--input path] [--output directory]"
                      << " [--limit rows] [--threads 1,2,4,8] [--repeats count]\n";
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown option: " + argument);
        }
    }
    if (options.input.empty() || options.outputDirectory.empty()) {
        throw std::runtime_error("--input and --output paths must not be empty.");
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        omp_set_dynamic(0);
        const Options options = parseOptions(argc, argv);
        const Dataset dataset = loadDataset(options.input, options.limit);
        if (dataset.orders.empty()) {
            throw std::runtime_error("The CSV contains no data rows.");
        }
        std::cout << "Loaded " << dataset.orders.size() << " rows from "
                  << options.input << " in " << std::fixed << std::setprecision(3)
                  << dataset.loadSeconds << " s\n";

        std::string outputDirectory = options.outputDirectory;
        if (!outputDirectory.empty()
                && outputDirectory[outputDirectory.size() - 1] != '\\'
                && outputDirectory[outputDirectory.size() - 1] != '/') {
            outputDirectory += "\\";
        }
        const std::string summaryPath = outputDirectory + "summary.csv";
        const std::string benchmarkPath = outputDirectory + "benchmark.csv";
        const std::string graphPath = outputDirectory + "performance.svg";
        const std::string reportPath = outputDirectory + "performance_summary.txt";
        if (!ensureOutputDirectory(options.outputDirectory)) {
            throw std::runtime_error("Cannot create output directory: " + options.outputDirectory);
        }

        const Stats fullSequential = aggregateSequential(dataset, dataset.orders.size());
        writeSummary(summaryPath, fullSequential, dataset);
        std::cout << "Sequential: " << fullSequential.seconds << " s\n";

        std::ofstream benchmark(benchmarkPath.c_str());
        if (!benchmark) {
            throw std::runtime_error("Cannot create benchmark output: " + benchmarkPath);
        }
        benchmark << "rows,threads,sequential_seconds,parallel_seconds,speedup,efficiency\n";
        benchmark << std::setprecision(9);
        std::vector<BenchmarkResult> benchmarkResults;
        const std::vector<std::size_t> sizes = benchmarkSizes(dataset.orders.size());
        for (std::size_t sizeIndex = 0; sizeIndex < sizes.size(); ++sizeIndex) {
            const std::size_t size = sizes[sizeIndex];
            const Stats expected = aggregateSequential(dataset, size);
            const double sequentialSeconds =
                    medianSequential(dataset, size, options.repeats) / 1e9;
            for (std::size_t threadIndex = 0;
                    threadIndex < options.threads.size(); ++threadIndex) {
                const int threads = options.threads[threadIndex];
                const Stats parallel = aggregateParallel(dataset, size, threads);
                verifyEquivalent(expected, parallel, threads);
                const double parallelSeconds =
                        medianParallel(dataset, size, threads, options.repeats) / 1e9;
                const double speedup = parallelSeconds == 0.0
                        ? 0.0 : sequentialSeconds / parallelSeconds;
                const double efficiency = speedup / threads;
                benchmark << size << "," << threads << ","
                          << sequentialSeconds << "," << parallelSeconds << ","
                          << speedup << "," << efficiency << "\n";
                benchmark.flush();
                BenchmarkResult result = {
                    size, threads, sequentialSeconds, parallelSeconds, speedup, efficiency
                };
                benchmarkResults.push_back(result);
                std::cout << "Rows " << size << " | threads " << threads
                          << " | seq " << std::setprecision(6) << sequentialSeconds
                          << " s | parallel " << parallelSeconds
                          << " s | speedup " << std::setprecision(3) << speedup
                          << "x | efficiency " << efficiency * 100.0 << "%\n";
            }
        }
        writePerformanceArtifacts(
                graphPath, reportPath, benchmarkResults, options.threads);
        std::cout << "Correctness check: passed for every benchmark configuration.\n"
                  << "Summary: " << summaryPath << "\n"
                  << "Benchmark: " << benchmarkPath << "\n"
                  << "Graph: " << graphPath << "\n"
                  << "Performance analysis: " << reportPath << "\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Error: " << exception.what() << "\n";
        return 1;
    }
}
