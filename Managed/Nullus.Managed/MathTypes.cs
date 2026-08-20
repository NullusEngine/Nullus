using System;
using System.Runtime.InteropServices;

namespace Nullus;

[StructLayout(LayoutKind.Sequential)]
public struct Vector2(float x, float y)
{
    public float X = x, Y = y;
    public float x { readonly get => X; set => X = value; }
    public float y { readonly get => Y; set => Y = value; }
    public static Vector2 zero => new(0, 0);
    public static Vector2 one => new(1, 1);
    public float magnitude => MathF.Sqrt(X * X + Y * Y);
    public float sqrMagnitude => X * X + Y * Y;
    public Vector2 normalized => magnitude <= float.Epsilon ? zero : this / magnitude;
    public static Vector2 operator +(Vector2 a, Vector2 b) => new(a.X + b.X, a.Y + b.Y);
    public static Vector2 operator -(Vector2 a, Vector2 b) => new(a.X - b.X, a.Y - b.Y);
    public static Vector2 operator *(Vector2 a, float b) => new(a.X * b, a.Y * b);
    public static Vector2 operator /(Vector2 a, float b) => new(a.X / b, a.Y / b);
    public static float Dot(Vector2 a, Vector2 b) => a.X * b.X + a.Y * b.Y;
    public static float Distance(Vector2 a, Vector2 b) => (a - b).magnitude;
}
[StructLayout(LayoutKind.Sequential)]
public struct Vector3(float x, float y, float z)
{
    public float X = x, Y = y, Z = z;
    public float x { readonly get => X; set => X = value; }
    public float y { readonly get => Y; set => Y = value; }
    public float z { readonly get => Z; set => Z = value; }
    public static Vector3 zero => new(0, 0, 0);
    public static Vector3 one => new(1, 1, 1);
    public static Vector3 up => new(0, 1, 0);
    public static Vector3 down => new(0, -1, 0);
    public static Vector3 right => new(1, 0, 0);
    public static Vector3 left => new(-1, 0, 0);
    public static Vector3 forward => new(0, 0, 1);
    public static Vector3 back => new(0, 0, -1);
    public float magnitude => MathF.Sqrt(X * X + Y * Y + Z * Z);
    public float sqrMagnitude => X * X + Y * Y + Z * Z;
    public Vector3 normalized => magnitude <= float.Epsilon ? zero : this / magnitude;
    public static Vector3 operator +(Vector3 a, Vector3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vector3 operator -(Vector3 a, Vector3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vector3 operator -(Vector3 a) => new(-a.X, -a.Y, -a.Z);
    public static Vector3 operator *(Vector3 a, float b) => new(a.X * b, a.Y * b, a.Z * b);
    public static Vector3 operator *(float b, Vector3 a) => a * b;
    public static Vector3 operator /(Vector3 a, float b) => new(a.X / b, a.Y / b, a.Z / b);
    public static Vector3 operator /(Vector3 a, Vector3 b) => new(a.X / b.X, a.Y / b.Y, a.Z / b.Z);
    public static Vector3 Scale(Vector3 a, Vector3 b) => new(a.X * b.X, a.Y * b.Y, a.Z * b.Z);
    public static float Dot(Vector3 a, Vector3 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z;
    public static Vector3 Cross(Vector3 a, Vector3 b) => new(a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X);
    public static float Distance(Vector3 a, Vector3 b) => (a - b).magnitude;
    public static float Angle(Vector3 from, Vector3 to)
    {
        var denominator = MathF.Sqrt(from.sqrMagnitude * to.sqrMagnitude);
        return denominator <= float.Epsilon ? 0.0f : MathF.Acos(Math.Clamp(Dot(from, to) / denominator, -1.0f, 1.0f)) * Mathf.Rad2Deg;
    }
    public static Vector3 Lerp(Vector3 a, Vector3 b, float t) => a + (b - a) * Mathf.Clamp01(t);
    public static Vector3 Reflect(Vector3 direction, Vector3 normal) => direction - 2.0f * Dot(direction, normal) * normal;
    public static Vector3 Project(Vector3 vector, Vector3 onNormal)
    {
        var denominator = onNormal.sqrMagnitude;
        return denominator <= float.Epsilon ? zero : onNormal * (Dot(vector, onNormal) / denominator);
    }
}
[StructLayout(LayoutKind.Sequential)]
public struct Vector4(float x, float y, float z, float w)
{
    public float X = x, Y = y, Z = z, W = w;
    public float x { readonly get => X; set => X = value; }
    public float y { readonly get => Y; set => Y = value; }
    public float z { readonly get => Z; set => Z = value; }
    public float w { readonly get => W; set => W = value; }
    public static Vector4 zero => new(0, 0, 0, 0);
    public static Vector4 one => new(1, 1, 1, 1);
    public static Vector4 operator +(Vector4 a, Vector4 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z, a.W + b.W);
    public static Vector4 operator -(Vector4 a, Vector4 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z, a.W - b.W);
    public static Vector4 operator -(Vector4 a) => new(-a.X, -a.Y, -a.Z, -a.W);
    public static Vector4 operator *(Vector4 a, float b) => new(a.X * b, a.Y * b, a.Z * b, a.W * b);
    public static Vector4 operator /(Vector4 a, float b) => new(a.X / b, a.Y / b, a.Z / b, a.W / b);
    public float magnitude => MathF.Sqrt(X * X + Y * Y + Z * Z + W * W);
    public float sqrMagnitude => X * X + Y * Y + Z * Z + W * W;
    public Vector4 normalized => magnitude <= float.Epsilon ? zero : this / magnitude;
    public static float Dot(Vector4 a, Vector4 b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
}
[StructLayout(LayoutKind.Sequential)]
public struct Quaternion(float x, float y, float z, float w)
{
    public float X = x, Y = y, Z = z, W = w;
    public float x { readonly get => X; set => X = value; }
    public float y { readonly get => Y; set => Y = value; }
    public float z { readonly get => Z; set => Z = value; }
    public float w { readonly get => W; set => W = value; }
    public static Quaternion identity => new(0, 0, 0, 1);
    public float magnitude => MathF.Sqrt(X * X + Y * Y + Z * Z + W * W);
    public Quaternion normalized => magnitude <= float.Epsilon ? identity : new Quaternion(X / magnitude, Y / magnitude, Z / magnitude, W / magnitude);
    public Vector3 eulerAngles
    {
        get
        {
            var q = normalized;
            var sinRollCosPitch = 2.0f * (q.W * q.X + q.Y * q.Z);
            var cosRollCosPitch = 1.0f - 2.0f * (q.X * q.X + q.Y * q.Y);
            var roll = MathF.Atan2(sinRollCosPitch, cosRollCosPitch);
            var sinPitch = Math.Clamp(2.0f * (q.W * q.Y - q.Z * q.X), -1.0f, 1.0f);
            var pitch = MathF.Asin(sinPitch);
            var sinYawCosPitch = 2.0f * (q.W * q.Z + q.X * q.Y);
            var cosYawCosPitch = 1.0f - 2.0f * (q.Y * q.Y + q.Z * q.Z);
            var yaw = MathF.Atan2(sinYawCosPitch, cosYawCosPitch);
            return new Vector3(roll, pitch, yaw) * Mathf.Rad2Deg;
        }
    }

    public static bool operator ==(Quaternion a, Quaternion b) => MathF.Abs(Dot(a.normalized, b.normalized)) >= 1.0f - 1e-6f;
    public static bool operator !=(Quaternion a, Quaternion b) => !(a == b);
    public override readonly bool Equals(object? obj) => obj is Quaternion other && this == other;
    public override readonly int GetHashCode() => HashCode.Combine(X, Y, Z, W);

    public static Quaternion operator *(Quaternion a, Quaternion b) => new(
        a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y,
        a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X,
        a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W,
        a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z);

    public static Vector3 operator *(Quaternion q, Vector3 v)
    {
        var t = 2.0f * Vector3.Cross(new Vector3(q.X, q.Y, q.Z), v);
        return v + q.W * t + Vector3.Cross(new Vector3(q.X, q.Y, q.Z), t);
    }

    public static Quaternion AngleAxis(float degrees, Vector3 axis)
    {
        var length = MathF.Sqrt(axis.X * axis.X + axis.Y * axis.Y + axis.Z * axis.Z);
        if (length <= float.Epsilon)
            return new Quaternion(0, 0, 0, 1);
        var halfRadians = degrees * (MathF.PI / 180.0f) * 0.5f;
        var scale = MathF.Sin(halfRadians) / length;
        return new Quaternion(
            axis.X * scale,
            axis.Y * scale,
            axis.Z * scale,
            MathF.Cos(halfRadians));
    }

    public static Quaternion Euler(Vector3 degrees)
    {
        var half = degrees * (MathF.PI / 360.0f);
        var sx = MathF.Sin(half.X); var cx = MathF.Cos(half.X);
        var sy = MathF.Sin(half.Y); var cy = MathF.Cos(half.Y);
        var sz = MathF.Sin(half.Z); var cz = MathF.Cos(half.Z);
        return new Quaternion(
            sx * cy * cz - cx * sy * sz,
            cx * sy * cz + sx * cy * sz,
            cx * cy * sz - sx * sy * cz,
            cx * cy * cz + sx * sy * sz);
    }

    public static Quaternion LookRotation(Vector3 forward, Vector3 up)
    {
        forward = forward.normalized;
        if (forward.sqrMagnitude <= float.Epsilon)
            return identity;
        var right = Vector3.Cross(up, forward).normalized;
        up = Vector3.Cross(forward, right);
        var trace = right.X + up.Y + forward.Z;
        if (trace > 0)
        {
            var s = MathF.Sqrt(trace + 1) * 2;
            return new Quaternion((up.Z - forward.Y) / s, (forward.X - right.Z) / s, (right.Y - up.X) / s, 0.25f * s);
        }
        return identity;
    }

    public static Quaternion Inverse(Quaternion value)
    {
        var norm = value.X * value.X + value.Y * value.Y + value.Z * value.Z + value.W * value.W;
        return norm <= float.Epsilon ? identity : new Quaternion(-value.X / norm, -value.Y / norm, -value.Z / norm, value.W / norm);
    }
    public static float Dot(Quaternion a, Quaternion b) => a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    public static float Angle(Quaternion a, Quaternion b) => MathF.Acos(Math.Clamp(MathF.Abs(Dot(a.normalized, b.normalized)), -1, 1)) * 2 * (180 / MathF.PI);
    public static Quaternion Lerp(Quaternion a, Quaternion b, float t) => new Quaternion(
        a.X + (b.X - a.X) * Mathf.Clamp01(t),
        a.Y + (b.Y - a.Y) * Mathf.Clamp01(t),
        a.Z + (b.Z - a.Z) * Mathf.Clamp01(t),
        a.W + (b.W - a.W) * Mathf.Clamp01(t)).normalized;
    public static Quaternion Slerp(Quaternion a, Quaternion b, float t)
    {
        t = Mathf.Clamp01(t);
        var dot = Dot(a.normalized, b.normalized);
        if (dot < 0.0f) { b = new Quaternion(-b.X, -b.Y, -b.Z, -b.W); dot = -dot; }
        if (dot > 0.9995f) return Lerp(a, b, t);
        var theta = MathF.Acos(Math.Clamp(dot, -1.0f, 1.0f));
        var sinTheta = MathF.Sin(theta);
        var wa = MathF.Sin((1.0f - t) * theta) / sinTheta;
        var wb = MathF.Sin(t * theta) / sinTheta;
        return new Quaternion(a.X * wa + b.X * wb, a.Y * wa + b.Y * wb, a.Z * wa + b.Z * wb, a.W * wa + b.W * wb).normalized;
    }
}
[StructLayout(LayoutKind.Sequential)]
public struct Color(float r, float g, float b, float a = 1)
{
    public float R = r, G = g, B = b, A = a;
    public float r { readonly get => R; set => R = value; }
    public float g { readonly get => G; set => G = value; }
    public float b { readonly get => B; set => B = value; }
    public float a { readonly get => A; set => A = value; }
    public static Color white => new(1, 1, 1, 1);
    public static Color black => new(0, 0, 0, 1);
    public static Color clear => new(0, 0, 0, 0);
    public static Color operator +(Color a, Color b) => new(a.R + b.R, a.G + b.G, a.B + b.B, a.A + b.A);
    public static Color operator *(Color a, float b) => new(a.R * b, a.G * b, a.B * b, a.A * b);
    public static Color Lerp(Color a, Color b, float t) => new(
        a.R + (b.R - a.R) * Mathf.Clamp01(t),
        a.G + (b.G - a.G) * Mathf.Clamp01(t),
        a.B + (b.B - a.B) * Mathf.Clamp01(t),
        a.A + (b.A - a.A) * Mathf.Clamp01(t));
}

public readonly struct Ray(Vector3 origin, Vector3 direction)
{
    public Vector3 origin { get; } = origin;
    public Vector3 direction { get; } = direction;
    public Vector3 GetPoint(float distance) => origin + direction * distance;
}

public static class Mathf
{
    public const float PI = MathF.PI;
    public const float Deg2Rad = MathF.PI / 180.0f;
    public const float Rad2Deg = 180.0f / MathF.PI;
    public const float Epsilon = 1.401298E-45f;
    public static float Abs(float value) => MathF.Abs(value);
    public static float Min(float a, float b) => MathF.Min(a, b);
    public static float Max(float a, float b) => MathF.Max(a, b);
    public static float Clamp(float value, float min, float max) => Math.Clamp(value, min, max);
    public static int Clamp(int value, int min, int max) => Math.Clamp(value, min, max);
    public static float Clamp01(float value) => Math.Clamp(value, 0, 1);
    public static float Lerp(float a, float b, float t) => a + (b - a) * Clamp01(t);
    public static float LerpUnclamped(float a, float b, float t) => a + (b - a) * t;
    public static float MoveTowards(float current, float target, float maxDelta) => MathF.Abs(target - current) <= maxDelta ? target : current + MathF.Sign(target - current) * maxDelta;
    public static float Sin(float value) => MathF.Sin(value);
    public static float Cos(float value) => MathF.Cos(value);
    public static float Sqrt(float value) => MathF.Sqrt(value);
    public static float Pow(float value, float power) => MathF.Pow(value, power);
    public static float Tan(float value) => MathF.Tan(value);
    public static float Atan2(float y, float x) => MathF.Atan2(y, x);
    public static float Floor(float value) => MathF.Floor(value);
    public static float Ceil(float value) => MathF.Ceiling(value);
    public static float Round(float value) => MathF.Round(value);
    public static int Sign(float value) => MathF.Sign(value);
    public static float InverseLerp(float a, float b, float value) => MathF.Abs(a - b) <= Epsilon ? 0 : Clamp01((value - a) / (b - a));
    public static float SmoothStep(float from, float to, float t)
    {
        if (MathF.Abs(to - from) <= Epsilon)
            return to;
        t = Clamp01((t - from) / (to - from));
        return t * t * (3 - 2 * t);
    }
    public static bool Approximately(float a, float b) => MathF.Abs(b - a) < MathF.Max(1e-6f * MathF.Max(MathF.Abs(a), MathF.Abs(b)), Epsilon * 8);
    public static float Repeat(float t, float length) => t - MathF.Floor(t / length) * length;
}
