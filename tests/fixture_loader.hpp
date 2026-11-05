#pragma once

#include "vision/tensor_view.hpp"

#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace so_test {

/// Owns the bytes behind a set of TensorViews loaded from tools/dump_fixtures.py.
class FixtureTensors {
public:
    explicit FixtureTensors(const std::string& manifest_path) {
        const std::string dir = manifest_path.substr(0, manifest_path.find_last_of('/') + 1);
        std::ifstream mf(manifest_path);
        if (!mf) throw std::runtime_error("cannot open fixture manifest " + manifest_path);

        std::string line;
        while (std::getline(mf, line)) {
            if (line.empty()) continue;
            std::istringstream ls(line);
            int index = 0, ndim = 0;
            ls >> index >> ndim;
            std::vector<std::int64_t> shape(static_cast<std::size_t>(ndim));
            std::size_t elems = 1;
            for (int d = 0; d < ndim; ++d) {
                ls >> shape[static_cast<std::size_t>(d)];
                elems *= static_cast<std::size_t>(shape[static_cast<std::size_t>(d)]);
            }
            std::string file;
            ls >> file;

            auto buf = std::make_unique<std::vector<float>>(elems);
            std::ifstream bf(dir + file, std::ios::binary);
            if (!bf) throw std::runtime_error("cannot open fixture tensor " + dir + file);
            bf.read(reinterpret_cast<char*>(buf->data()),
                    static_cast<std::streamsize>(elems * sizeof(float)));
            if (static_cast<std::size_t>(bf.gcount()) != elems * sizeof(float))
                throw std::runtime_error("short read on fixture tensor " + file);

            views_.push_back(so::TensorView{shape, buf->data()});
            storage_.push_back(std::move(buf));
        }
        if (views_.empty()) throw std::runtime_error("empty fixture manifest " + manifest_path);
    }

    const std::vector<so::TensorView>& views() const { return views_; }

private:
    std::vector<std::unique_ptr<std::vector<float>>> storage_;
    std::vector<so::TensorView> views_;
};

/// A fixture read whole, as text.
///
/// Used for the captured ONVIF responses, which are XML a camera really sent
/// rather than anything generated here, and are therefore kept verbatim.
inline std::string load_fixture_text(const std::string& name) {
    const std::string path = "tests/fixtures/" + name;
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// One row of the ORT ground-truth CSV: box(4), score, class, then 17x(x,y,conf).
struct ExpectedRow {
    float box[4];
    float score;
    float cls;
    float kps[17][3];
};

inline std::vector<ExpectedRow> load_expected(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<ExpectedRow> rows;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<float> v;
        std::istringstream ls(line);
        std::string cell;
        while (std::getline(ls, cell, ',')) v.push_back(std::stof(cell));
        if (v.size() != 57)
            throw std::runtime_error("expected 57 columns per reference row, got " +
                                     std::to_string(v.size()));
        ExpectedRow r{};
        for (int i = 0; i < 4; ++i) r.box[i] = v[static_cast<std::size_t>(i)];
        r.score = v[4];
        r.cls = v[5];
        for (int k = 0; k < 17; ++k)
            for (int c = 0; c < 3; ++c)
                r.kps[k][c] = v[static_cast<std::size_t>(6 + k * 3 + c)];
        rows.push_back(r);
    }
    return rows;
}

/// One row of the SCRFD reference CSV: box(4), score, then 5x(x,y).
struct ExpectedFace {
    float box[4];
    float score;
    float lm[5][2];
};

inline std::vector<ExpectedFace> load_expected_faces(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<ExpectedFace> rows;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<float> v;
        std::istringstream ls(line);
        std::string cell;
        while (std::getline(ls, cell, ',')) v.push_back(std::stof(cell));
        if (v.size() != 15)
            throw std::runtime_error("expected 15 columns per reference face, got " +
                                     std::to_string(v.size()));
        ExpectedFace r{};
        for (int i = 0; i < 4; ++i) r.box[i] = v[static_cast<std::size_t>(i)];
        r.score = v[4];
        for (int k = 0; k < 5; ++k)
            for (int c = 0; c < 2; ++c)
                r.lm[k][c] = v[static_cast<std::size_t>(5 + k * 2 + c)];
        rows.push_back(r);
    }
    return rows;
}

}  // namespace so_test
