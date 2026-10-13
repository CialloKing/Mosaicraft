#pragma once
#include <algorithm>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace mosaicraft
{
// 行视图不持有内存；完整特征存储连续，供 CPU 访问和 CUDA 上传共同使用。
template <class T, std::size_t Columns> class FeatureMatrix
{
  public:
    template <class Element> class Row
    {
      public:
        explicit Row(Element *values) : m_values(values)
        {
        }
        std::size_t size() const
        {
            return Columns;
        }
        Element *data() const
        {
            return m_values;
        }
        Element &operator[](std::size_t i) const
        {
            return m_values[i];
        }
        Element *begin() const
        {
            return m_values;
        }
        Element *end() const
        {
            return m_values + Columns;
        }
        Row &operator=(const std::vector<T> &source)
        {
            std::copy_n(source.data(), Columns, m_values);
            return *this;
        }
        operator std::vector<T>() const
        {
            return std::vector<T>(begin(), end());
        }

      private:
        Element *m_values;
    };
    explicit FeatureMatrix(std::size_t rows) : m_values(elements(rows))
    {
    }
    Row<T> operator[](std::size_t row)
    {
        return Row<T>(m_values.data() + row * Columns);
    }
    Row<const T> operator[](std::size_t row) const
    {
        return Row<const T>(m_values.data() + row * Columns);
    }
    T *data()
    {
        return m_values.data();
    }
    void resize(std::size_t rows)
    {
        m_values.resize(elements(rows));
    }
    std::size_t capacityBytes() const
    {
        return m_values.capacity() * sizeof(T);
    }

  private:
    static std::size_t elements(std::size_t rows)
    {
        if (rows > std::numeric_limits<std::size_t>::max() / Columns)
        {
            throw std::overflow_error("feature matrix size overflow");
        }
        return rows * Columns;
    }
    std::vector<T> m_values;
};
} // namespace mosaicraft
