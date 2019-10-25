/******************************************************************************
 *
 * Copyright (c) 2019, the Perspective Authors.
 *
 * This file is part of the Perspective library, distributed under the terms of
 * the Apache License 2.0.  The full license can be found in the LICENSE file.
 *
 */
#ifdef PSP_ENABLE_PYTHON
#include <perspective/python/fill.h>
#include <perspective/python/numpy.h>

using namespace perspective;

namespace perspective {
namespace numpy {

    NumpyLoader::NumpyLoader(py::object accessor)
        : m_init(false)
        , m_accessor(accessor) {}

    NumpyLoader::~NumpyLoader() {}

    void
    NumpyLoader::init() {
        m_names = make_names();
        m_types = make_types();
        m_init = true;
    }

    std::vector<std::string>
    NumpyLoader::names() const {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        return m_names;
    }

    std::vector<t_dtype>
    NumpyLoader::types() const {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        return m_types;
    }

    std::uint32_t
    NumpyLoader::row_count() const {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        return m_accessor.attr("row_count")().cast<std::uint32_t>();
    }

    void
    NumpyLoader::fill_table(t_data_table& tbl, const t_schema& input_schema,
        const std::string& index, std::uint32_t offset, std::uint32_t limit, bool is_update) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        bool implicit_index = false;
        std::vector<std::string> col_names(input_schema.columns());
        std::vector<t_dtype> data_types(input_schema.types());

        for (auto cidx = 0; cidx < col_names.size(); ++cidx) {
            auto name = col_names[cidx];
            auto type = data_types[cidx];

            if (name == "__INDEX__") {
                implicit_index = true;
                std::shared_ptr<t_column> pkey_col_sptr = tbl.add_column_sptr("psp_pkey", type, true);
                fill_column(tbl, pkey_col_sptr, "__INDEX__", type, cidx, is_update);
                tbl.clone_column("psp_pkey", "psp_okey");
                continue;
            }

            auto col = tbl.get_column(name);
            fill_column(tbl, col, name, type, cidx, is_update);
        }

        // Fill index column - recreated every time a `t_data_table` is created.
        if (!implicit_index) {
            if (index == "") {
                // Use row number as index if not explicitly provided or provided with `__INDEX__`
                auto key_col = tbl.add_column("psp_pkey", DTYPE_INT32, true);
                auto okey_col = tbl.add_column("psp_okey", DTYPE_INT32, true);

                for (std::uint32_t ridx = 0; ridx < tbl.size(); ++ridx) {
                    key_col->set_nth<std::int32_t>(ridx, (ridx + offset) % limit);
                    okey_col->set_nth<std::int32_t>(ridx, (ridx + offset) % limit);
                }
            } else {
                tbl.clone_column(index, "psp_pkey");
                tbl.clone_column(index, "psp_okey");
            }
        }
    }

    
    void 
    NumpyLoader::fill_column(t_data_table& tbl, std::shared_ptr<t_column> col, const std::string& name, t_dtype type, std::uint32_t cidx, bool is_update) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");

        // Use name index instead of column index - prevents off-by-one errors with the "index" column.
        auto name_it = std::find(m_names.begin(), m_names.end(), name); 
        
        if (name_it == m_names.end()) {
            std::stringstream ss;
            ss << "Cannot fill column '" << name << "' as it is not in the table schema.";
            PSP_COMPLAIN_AND_ABORT(ss.str());
        }

        auto nidx = std::distance(m_names.begin(), name_it);
        
        py::dict source = m_accessor.attr("_get_numpy_column")(name, type);
        py::array array = py::array::ensure(source["array"].cast<py::object>());
        py::array_t<std::uint64_t> mask = source["mask"].cast<py::array_t<std::uint64_t>>();

        if (!array) {
            std::stringstream ss;
            ss << "Cannot fill a non-numpy array at column '" << name;
            PSP_COMPLAIN_AND_ABORT(ss.str());
        }

        /**
         * np_dtype is used to attempt `memcpy` of the numpy array into perspective.
         * if memcpy fails, it is most likely a numpy array with `dtype=object`, so use the inferred type from perspective.
         */
        t_dtype np_dtype = m_types[nidx];

        /**
         * Catch common type mismatches that occur when a numpy dtype is of greater bit width than the Perspective t_dtype:
         * - if `np_dtype` is int64 and `t_dtype` is `DTYPE_INT32`, fill iteratively.
         * - if `np_dtype` is int64 and `t_dtype` is `DTYPE_FLOAT64`, fill iteratively.
         * These errors occur frqeuently when a Table is created from non-numpy data, then updated with a numpy array.
         * In these cases, the `t_dtype` of the Table supercedes the array dtype.
         */
        if (np_dtype == DTYPE_INT64 && (type == DTYPE_INT32 || type == DTYPE_FLOAT64)) {
            std::cout << "np dtype: " << dtype_to_str(np_dtype) << ", dtype: " << dtype_to_str(type) << std::endl;
            fill_column_iter(array, tbl, col, name, np_dtype, type, cidx, is_update);
            return;
        }

        // `copy_array` returns a status - FILL_SUCCEED or FILL_FAIL
        t_fill_status filled = copy_array(array, col, np_dtype, 0);

        if (filled == FILL_SUCCEED) {
            // Fill validity map
            col->valid_raw_fill();
            auto num_invalid = mask.size();

            if (num_invalid > 0) {
                std::uint64_t* ptr = (std::uint64_t*) mask.data();
                for (auto i = 0; i < num_invalid; ++i) {
                    std::uint64_t idx = ptr[i];
                    if (is_update) {
                        col->unset(idx);
                    } else {
                        col->clear(idx);
                    }
                }
            }
        } else {
            // Array could not be copied - fill iteratively
            fill_column_iter(array, tbl, col, name, np_dtype, type, cidx, is_update);
        }
    }

    void
    NumpyLoader::fill_column_iter(py::array array, t_data_table& tbl, std::shared_ptr<t_column> col, const std::string& name, t_dtype np_dtype, t_dtype type, std::uint32_t cidx, bool is_update) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        switch (type) {
            case DTYPE_TIME: {
                fill_datetime_iter(array, col, name, np_dtype, type, cidx, is_update);
            } break;
            case DTYPE_DATE: {
                fill_date_iter(array, col, name, np_dtype, type, cidx, is_update);
            } break;
            case DTYPE_BOOL: {
                fill_bool_iter(array, col, name, np_dtype, type, cidx, is_update);
            } break;
            case DTYPE_STR: {
                fill_string_iter(array, col, name, np_dtype, type, cidx, is_update);
            } break;
            default: {
                fill_numeric_iter(array, tbl, col, name, np_dtype, type, cidx, is_update);
                break;
            }
        }
    }

    void 
    NumpyLoader::fill_numeric_iter(py::array array, t_data_table& tbl, std::shared_ptr<t_column> col, const std::string& name, t_dtype np_dtype, t_dtype type, std::uint32_t cidx, bool is_update) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        t_uindex nrows = col->size();
        const void* ptr = array.data();

        for (auto i = 0; i < nrows; ++i) {
            if (npy_isnan(((double*)ptr)[i])) {
                if (is_update) {
                    col->unset(i);
                } else {
                    col->clear(i);
                }
                continue;
            }

            switch (type) {
                case DTYPE_UINT8: {
                    col->set_nth(i, ((std::uint8_t*)ptr)[i]);
                } break;
                case DTYPE_UINT16: {
                    col->set_nth(i, ((std::uint16_t*)ptr)[i]);
                } break;
                case DTYPE_UINT32: {
                    col->set_nth(i, ((std::uint32_t*)ptr)[i]);
                } break;
                case DTYPE_UINT64: {
                    col->set_nth(i, ((std::uint64_t*)ptr)[i]);
                } break;
                case DTYPE_INT8: {
                    col->set_nth(i, ((std::int8_t*)ptr)[i]);
                } break;
                case DTYPE_INT16: {
                    col->set_nth(i, ((std::int16_t*)ptr)[i]);
                } break;
                case DTYPE_INT32: {
                    // This handles cases where a long sequence of e.g. 0 precedes a clearly
                    // float value in an inferred column. Would not be needed if the type
                    // inference checked the entire column/we could reset parsing.
                    double item = ((double*)ptr)[i];
                    if (item > 2147483647 || item < -2147483648) {
                        binding::WARN("Promoting %s to float from int32", name);
                        tbl.promote_column(name, DTYPE_FLOAT64, i, true);
                        col = tbl.get_column(name);
                        type = DTYPE_FLOAT64;
                        col->set_nth(i, item);
                    } else {
                        if (np_dtype == DTYPE_INT64) {
                            std::cout << ((std::int64_t*)ptr)[i] <<" - , ";
                            col->set_nth<std::int32_t>(i, ((std::int64_t*)ptr)[i]);
                        } else {
                            col->set_nth<std::int32_t>(i, ((std::int32_t*)ptr)[i]);
                        }
                    }
                } break;
                case DTYPE_INT64: {
                    std::int64_t item = ((std::int64_t*)ptr)[i];
                    if (npy_isnan(item)) {
                        binding::WARN("Promoting %s to string from int64", name);
                        tbl.promote_column(name, DTYPE_STR, i, false);
                        col = tbl.get_column(name);
                        fill_string_iter(array, col, name, np_dtype, DTYPE_STR, cidx, is_update);
                        return;
                    } else {
                        col->set_nth<std::int64_t>(i, item);
                    }
                } break;
                case DTYPE_FLOAT32: {
                    col->set_nth(i, ((float*)ptr)[i]);
                } break;
                case DTYPE_FLOAT64: {
                    double item = ((double*)ptr)[i];
                    std::int64_t i2 = ((std::int64_t*)ptr)[i];
                    std::cout << item << " (" << i2 << "), ";
                    if (npy_isnan(item)) {
                        binding::WARN("Promoting %s to string from int64", name);
                        tbl.promote_column(name, DTYPE_STR, i, false);
                        col = tbl.get_column(name);
                        fill_string_iter(array, col, name, np_dtype, DTYPE_STR, cidx, is_update);
                        return;
                    }
                    if (np_dtype == DTYPE_INT64) {
                        col->set_nth<double>(i, ((std::int64_t*)ptr)[i]);
                    } else {
                        col->set_nth<double>(i, item);
                    }
                } break;
                default:
                    break;
            }
        }
    }

    void
    NumpyLoader::fill_datetime_iter(py::array array, std::shared_ptr<t_column> col, const std::string& name, t_dtype np_dtype, t_dtype type, std::uint32_t cidx, bool is_update) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        t_uindex nrows = col->size();
        std::int64_t* ptr = (std::int64_t*) array.data();

        for (auto i = 0; i < nrows; ++i) {
            std::int64_t item = ptr[i] * 1000;

            if (npy_isnan(item)) {
                if (is_update) {
                    col->unset(i);
                } else {
                    col->clear(i);
                }
                continue;
            }

            col->set_nth<std::int64_t>(i, item); // convert to milliseconds         
        }
    }

    void
    NumpyLoader::fill_date_iter(py::array array, std::shared_ptr<t_column> col, const std::string& name, t_dtype np_dtype, t_dtype type, std::uint32_t cidx, bool is_update) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        t_uindex nrows = col->size();

        for (auto i = 0; i < nrows; ++i) {
            t_val item = m_accessor.attr("marshal")(cidx, i, type);

            if (item.is_none()) {
                if (is_update) {
                    col->unset(i);
                } else {
                    col->clear(i);
                }
                continue;
            }


            auto date_components = item.cast<std::map<std::string, std::int32_t>>();
            t_date dt = t_date(date_components["year"], date_components["month"], date_components["day"]);
            col->set_nth(i, dt);
        }
    }

    void
    NumpyLoader::fill_string_iter(py::array array, std::shared_ptr<t_column> col, const std::string& name, t_dtype np_dtype, t_dtype type, std::uint32_t cidx, bool is_update) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        t_uindex nrows = col->size();

        for (auto i = 0; i < nrows; ++i) {
            t_val item = m_accessor.attr("marshal")(cidx, i, type);

            if (item.is_none()) {
                if (is_update) {
                    col->unset(i);
                } else {
                    col->clear(i);
                }
                continue;
            }

            // convert to a python string first
            std::wstring welem = item.cast<std::wstring>();
            std::wstring_convert<utf16convert_type, wchar_t> converter;
            std::string elem = converter.to_bytes(welem);
            col->set_nth(i, elem);
        }
    }

    void
    NumpyLoader::fill_bool_iter(py::array array, std::shared_ptr<t_column> col, const std::string& name, t_dtype np_dtype, t_dtype type, std::uint32_t cidx, bool is_update) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        t_uindex nrows = col->size();

        for (auto i = 0; i < nrows; ++i) {
            t_val item = m_accessor.attr("marshal")(cidx, i, type);

            if (item.is_none()) {
                if (is_update) {
                    col->unset(i);
                } else {
                    col->clear(i);
                }
                continue;
            }

            col->set_nth(i, item.cast<bool>());
        }
    }

    t_fill_status
    NumpyLoader::copy_array(py::array src, std::shared_ptr<t_column> dest, t_dtype np_dtype, const std::uint64_t offset) {
        PSP_VERBOSE_ASSERT(m_init, "touching uninited object");
        std::int64_t length = src.size();

        switch (np_dtype) {
            case DTYPE_UINT8: {
                copy_array_helper<std::uint8_t>(src.data(), dest, offset);
            } break;
            case DTYPE_UINT16: {
                copy_array_helper<std::uint16_t>(src.data(), dest, offset);
            } break;
            case DTYPE_UINT32: {
                copy_array_helper<std::uint32_t>(src.data(), dest, offset);
            } break;
            case DTYPE_UINT64: {
                copy_array_helper<std::uint64_t>(src.data(), dest, offset);
            } break;
            case DTYPE_INT8: {
                copy_array_helper<std::int8_t>(src.data(), dest, offset);
            } break;
            case DTYPE_INT16: {
                copy_array_helper<std::int16_t>(src.data(), dest, offset);
            } break;
            case DTYPE_INT32: {
                copy_array_helper<std::int32_t>(src.data(), dest, offset);
            } break;
            case DTYPE_INT64: {
                copy_array_helper<std::int64_t>(src.data(), dest, offset);
            } break;
            case DTYPE_FLOAT32: {
                copy_array_helper<float>(src.data(), dest, offset);
            } break;
            case DTYPE_FLOAT64: {
                copy_array_helper<double>(src.data(), dest, offset);
            } break;
            default: {
                return t_fill_status::FILL_FAIL;
            }
        }
        
        return t_fill_status::FILL_SUCCEED;
    }

    template <typename T>
    void copy_array_helper(const void* src, std::shared_ptr<t_column> dest, const std::uint64_t offset) {
        std::memcpy(dest->get_nth<T>(offset), src, dest->size() * sizeof(T));
    }

    std::vector<std::string>
    NumpyLoader::make_names() {
        auto names = py::list(m_accessor.attr("data")().attr("keys")());
        return names.cast<std::vector<std::string>>();
    }

    std::vector<t_dtype>
    NumpyLoader::make_types() {
        std::vector<t_dtype> rval;
        
        py::list arrays = m_accessor.attr("data")().attr("values")();
        for (const auto& a : arrays) {
            py::array array = py::array::ensure(a);

            if (!array) {
                PSP_COMPLAIN_AND_ABORT("Cannot fill mixed dictionaries of numpy.array and list!");
            }

            if (py::isinstance<py::array_t<std::uint8_t>>(array)) {
                rval.push_back(DTYPE_UINT8);
            } else if (py::isinstance<py::array_t<std::uint16_t>>(array)) {
                rval.push_back(DTYPE_UINT16);
            } else if (py::isinstance<py::array_t<std::uint32_t>>(array)) {
                rval.push_back(DTYPE_UINT32);
            } else if (py::isinstance<py::array_t<std::uint64_t>>(array)) {
                rval.push_back(DTYPE_UINT64);
            } else if (py::isinstance<py::array_t<std::int8_t>>(array)) {
                rval.push_back(DTYPE_INT8);
            } else if (py::isinstance<py::array_t<std::int16_t>>(array)) {
                rval.push_back(DTYPE_INT16);
            } else if (py::isinstance<py::array_t<std::int32_t>>(array)) {
                rval.push_back(DTYPE_INT32);
            } else if (py::isinstance<py::array_t<std::int64_t>>(array)) {
                rval.push_back(DTYPE_INT64);
            } else if (py::isinstance<py::array_t<float>>(array)) {
                rval.push_back(DTYPE_FLOAT32);
            } else if (py::isinstance<py::array_t<double>>(array)) {
                rval.push_back(DTYPE_FLOAT64);
            } else if (py::isinstance<py::array_t<bool>>(array)) {
                rval.push_back(DTYPE_BOOL);
            } else {    
                rval.push_back(DTYPE_STR);
            }
        }

        return rval;
    }
    
} // namespace numpy
} // namespace perspective
#endif