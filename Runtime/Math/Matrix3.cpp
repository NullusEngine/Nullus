#include <string>
#include <stdexcept>
#include <cmath>
#include <cstring>
#include "Math/Matrix3.h"
using namespace NLS::Maths;

const Matrix3 Matrix3::Identity = Matrix3(1.0f, 0.0f, 0.0f,
                                          0.0f, 1.0f, 0.0f,
                                          0.0f, 0.0f, 1.0f);

Matrix3::Matrix3()
{
    memcpy(data, Identity.data, 9 * sizeof(float));
}

Matrix3::Matrix3(float p_value)
{
    for (float& element : data)
        element = p_value;
}

Matrix3::Matrix3(float p_element1, float p_element2, float p_element3, float p_element4, float p_element5, float p_element6, float p_element7, float p_element8, float p_element9)
{
    data[0] = p_element1;
    data[1] = p_element2;
    data[2] = p_element3;
    data[3] = p_element4;
    data[4] = p_element5;
    data[5] = p_element6;
    data[6] = p_element7;
    data[7] = p_element8;
    data[8] = p_element9;
}

Matrix3::Matrix3(const Matrix3& p_other)
{
    *this = p_other;
}

Matrix3& Matrix3::operator=(const Matrix3& p_other)
{
    memcpy(this->data, p_other.data, 9 * sizeof(float));
    return *this;
}

bool Matrix3::operator==(const Matrix3& p_other)
{
    return AreEquals(*this, p_other);
}

Matrix3 Matrix3::operator+(const Matrix3& p_other) const
{
    return Add(*this, p_other);
}

Matrix3& Matrix3::operator+=(const Matrix3& p_other)
{
    *this = Add(*this, p_other);
    return *this;
}

Matrix3 Matrix3::operator-(const Matrix3& p_other) const
{
    return Subtract(*this, p_other);
}

Matrix3& Matrix3::operator-=(const Matrix3& p_other)
{
    *this = Subtract(*this, p_other);
    return *this;
}

Matrix3 Matrix3::operator*(float p_scalar) const
{
    return Multiply(*this, p_scalar);
}

Matrix3& Matrix3::operator*=(float p_scalar)
{
    *this = Multiply(*this, p_scalar);
    return *this;
}

Vector3 Matrix3::operator*(const Vector3& p_vector) const
{
    return Multiply(*this, p_vector);
}

Matrix3 Matrix3::operator*(const Matrix3& p_other) const
{
    return Multiply(*this, p_other);
}

Matrix3& Matrix3::operator*=(const Matrix3& p_other)
{
    *this = Multiply(*this, p_other);
    return *this;
}

Matrix3 Matrix3::operator/(float p_scalar) const
{
    return Divide(*this, p_scalar);
}

Matrix3& Matrix3::operator/=(float p_scalar)
{
    *this = Divide(*this, p_scalar);
    return *this;
}

Matrix3 Matrix3::operator/(const Matrix3& p_other) const
{
    return Divide(*this, p_other);
}

Matrix3& Matrix3::operator/=(const Matrix3& p_other)
{
    *this = Divide(*this, p_other);
    return *this;
}

float& Matrix3::operator()(uint8_t p_row, uint8_t p_column)
{
    if (p_row >= 3 || p_column >= 3)
        throw std::out_of_range(
            "Invalid index : " + std::to_string(p_row) + "," + std::to_string(p_column) + " is out of range");
    return data[3 * p_row + p_column];
}

bool Matrix3::AreEquals(const Matrix3& p_left, const Matrix3& p_right)
{
    return std::memcmp(&p_left, &p_right, 9 * sizeof(float)) == 0;
}

Matrix3 Matrix3::Add(const Matrix3& p_left, float p_scalar)
{
    Matrix3 result(p_left);
    for (uint8_t i = 0; i < 9; ++i)
        result.data[i] += p_scalar;
    return result;
}

Matrix3 Matrix3::Add(const Matrix3& p_left, const Matrix3& p_right)
{
    Matrix3 result(p_left);
    for (uint8_t i = 0; i < 9; ++i)
        result.data[i] += p_right.data[i];
    return result;
}

Matrix3 Matrix3::Subtract(const Matrix3& p_left, float p_scalar)
{
    Matrix3 result(p_left);
    for (float& element : result.data)
    {
        element -= p_scalar;
    }
    return result;
}

Matrix3 Matrix3::Subtract(const Matrix3& p_left, const Matrix3& p_right)
{
    Matrix3 result(p_left);
    for (uint8_t i = 0; i < 9; ++i)
        result.data[i] -= p_right.data[i];
    return result;
}

Matrix3 Matrix3::Multiply(const Matrix3& p_left, float p_scalar)
{
    Matrix3 result(p_left);
    for (float& element : result.data)
    {
        element *= p_scalar;
    }
    return result;
}

Vector3 Matrix3::Multiply(const Matrix3& p_matrix, const Vector3& p_vector)
{
    Vector3 result;
    result.x = ((p_matrix.data[0] * p_vector.x) + (p_matrix.data[1] * p_vector.y) + (p_matrix.data[2] * p_vector.z));
    result.y = ((p_matrix.data[3] * p_vector.x) + (p_matrix.data[4] * p_vector.y) + (p_matrix.data[5] * p_vector.z));
    result.z = ((p_matrix.data[6] * p_vector.x) + (p_matrix.data[7] * p_vector.y) + (p_matrix.data[8] * p_vector.z));

    return result;
}

Matrix3 Matrix3::Multiply(const Matrix3& p_left, const Matrix3& p_right)
{
    return Matrix3(
        (p_left.data[0] * p_right.data[0]) + (p_left.data[1] * p_right.data[3]) + (p_left.data[2] * p_right.data[6]),
        (p_left.data[0] * p_right.data[1]) + (p_left.data[1] * p_right.data[4]) + (p_left.data[2] * p_right.data[7]),
        (p_left.data[0] * p_right.data[2]) + (p_left.data[1] * p_right.data[5]) + (p_left.data[2] * p_right.data[8]),

        (p_left.data[3] * p_right.data[0]) + (p_left.data[4] * p_right.data[3]) + (p_left.data[5] * p_right.data[6]),
        (p_left.data[3] * p_right.data[1]) + (p_left.data[4] * p_right.data[4]) + (p_left.data[5] * p_right.data[7]),
        (p_left.data[3] * p_right.data[2]) + (p_left.data[4] * p_right.data[5]) + (p_left.data[5] * p_right.data[8]),

        (p_left.data[6] * p_right.data[0]) + (p_left.data[7] * p_right.data[3]) + (p_left.data[8] * p_right.data[6]),
        (p_left.data[6] * p_right.data[1]) + (p_left.data[7] * p_right.data[4]) + (p_left.data[8] * p_right.data[7]),
        (p_left.data[6] * p_right.data[2]) + (p_left.data[7] * p_right.data[5]) + (p_left.data[8] * p_right.data[8]));
}

Matrix3 Matrix3::Divide(const Matrix3& p_left, float p_scalar)
{
    Matrix3 result(p_left);
    for (float& element : result.data)
    {
        element /= p_scalar;
    }
    return result;
}

Matrix3 Matrix3::Divide(const Matrix3& p_left, const Matrix3& p_right)
{
    return p_left * Inverse(p_right);
}

bool Matrix3::IsIdentity(const Matrix3& p_matrix)
{
    return std::memcmp(Identity.data, p_matrix.data, 9 * sizeof(float)) == 0;
}

float Matrix3::Determinant(const Matrix3& p_matrix)
{
    return p_matrix.data[0] * (p_matrix.data[4] * p_matrix.data[8] - p_matrix.data[5] * p_matrix.data[7])
           - p_matrix.data[3] * (p_matrix.data[1] * p_matrix.data[8] - p_matrix.data[2] * p_matrix.data[7])
           + p_matrix.data[6] * (p_matrix.data[1] * p_matrix.data[5] - p_matrix.data[2] * p_matrix.data[4]);
}

Matrix3 Matrix3::Transpose(const Matrix3& p_matrix)
{
    Matrix3 result;

    result.data[0] = p_matrix.data[0];
    result.data[1] = p_matrix.data[3];
    result.data[2] = p_matrix.data[6];

    result.data[3] = p_matrix.data[1];
    result.data[4] = p_matrix.data[4];
    result.data[5] = p_matrix.data[7];

    result.data[6] = p_matrix.data[2];
    result.data[7] = p_matrix.data[5];
    result.data[8] = p_matrix.data[8];

    return result;
}

Matrix3 Matrix3::Cofactor(const Matrix3& p_matrix)
{
    return Matrix3(
        ((p_matrix.data[4] * p_matrix.data[8]) - (p_matrix.data[5] * p_matrix.data[7])),  // 0
        -((p_matrix.data[3] * p_matrix.data[8]) - (p_matrix.data[5] * p_matrix.data[6])), // 1
        ((p_matrix.data[3] * p_matrix.data[7]) - (p_matrix.data[4] * p_matrix.data[6])),  // 2
        -((p_matrix.data[1] * p_matrix.data[8]) - (p_matrix.data[2] * p_matrix.data[7])), // 3
        ((p_matrix.data[0] * p_matrix.data[8]) - (p_matrix.data[2] * p_matrix.data[6])),  // 4
        -((p_matrix.data[0] * p_matrix.data[7]) - (p_matrix.data[1] * p_matrix.data[6])), // 5
        ((p_matrix.data[1] * p_matrix.data[5]) - (p_matrix.data[2] * p_matrix.data[4])),  // 6
        -((p_matrix.data[0] * p_matrix.data[5]) - (p_matrix.data[2] * p_matrix.data[3])), // 7
        ((p_matrix.data[0] * p_matrix.data[4]) - (p_matrix.data[1] * p_matrix.data[3]))); // 8
}

Matrix3 Matrix3::Minor(const Matrix3& p_matrix)
{
    return Matrix3(
        ((p_matrix.data[4] * p_matrix.data[8]) - (p_matrix.data[5] * p_matrix.data[7])),  // 0
        ((p_matrix.data[3] * p_matrix.data[8]) - (p_matrix.data[5] * p_matrix.data[6])),  // 1
        ((p_matrix.data[3] * p_matrix.data[7]) - (p_matrix.data[4] * p_matrix.data[6])),  // 2
        ((p_matrix.data[1] * p_matrix.data[8]) - (p_matrix.data[2] * p_matrix.data[7])),  // 3
        ((p_matrix.data[0] * p_matrix.data[8]) - (p_matrix.data[2] * p_matrix.data[6])),  // 4
        ((p_matrix.data[0] * p_matrix.data[7]) - (p_matrix.data[1] * p_matrix.data[6])),  // 5
        ((p_matrix.data[1] * p_matrix.data[5]) - (p_matrix.data[2] * p_matrix.data[4])),  // 6
        ((p_matrix.data[0] * p_matrix.data[5]) - (p_matrix.data[2] * p_matrix.data[3])),  // 7
        ((p_matrix.data[0] * p_matrix.data[4]) - (p_matrix.data[1] * p_matrix.data[3]))); // 8
}

Matrix3 Matrix3::Adjoint(const Matrix3& p_other)
{
    return Transpose(Cofactor(p_other));
}

Matrix3 Matrix3::Inverse(const Matrix3& p_matrix)
{
    const float determinant = Determinant(p_matrix);
    if (determinant == 0)
        throw std::logic_error("Division by 0");

    return Adjoint(p_matrix) / determinant;
}

Matrix3 Matrix3::Translation(const Vector2& p_translation)
{
    return Matrix3(1, 0, p_translation.x, 0, 1, p_translation.y, 0, 0, 1);
}

Matrix3 Matrix3::Translate(const Matrix3& p_matrix, const Vector2& p_translation)
{
    return p_matrix * Translation(p_translation);
}

Matrix3 Matrix3::Rotation(float p_rotation)
{
    return Matrix3(std::cos(p_rotation), -std::sin(p_rotation), 0, std::sin(p_rotation), std::cos(p_rotation), 0, 0, 0, 1);
}

Matrix3 Matrix3::Rotate(const Matrix3& p_matrix, float p_rotation)
{
    return p_matrix * Rotation(p_rotation);
}

Matrix3 Matrix3::Scaling(const Vector2& p_scale)
{
    return Matrix3(p_scale.x, 0, 0, 0, p_scale.y, 0, 0, 0, 1);
}

Matrix3 Matrix3::Scale(const Matrix3& p_matrix, const Vector2& p_scale)
{
    return p_matrix * Scaling(p_scale);
}

Vector3 Matrix3::GetRow(const Matrix3& p_matrix, uint8_t p_row)
{
    if (p_row >= 3)
        throw std::out_of_range("Invalid index : " + std::to_string(p_row) + " is out of range");

    return Vector3(p_matrix.data[p_row * 3], p_matrix.data[p_row * 3 + 1], p_matrix.data[p_row * 3 + 2]);
}

Vector3 Matrix3::GetColumn(const Matrix3& p_matrix, uint8_t p_column)
{
    if (p_column >= 3)
        throw std::out_of_range("Invalid index : " + std::to_string(p_column) + " is out of range");

    return Vector3(p_matrix.data[p_column + 6], p_matrix.data[p_column + 3], p_matrix.data[p_column]);
}