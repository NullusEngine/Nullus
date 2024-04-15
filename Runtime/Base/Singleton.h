#pragma once

// 用于不需要手动创建和删除的单例类，要求类不对任何其他单例类有依赖
template<class T>
class SingletonWithInst
{
public:
    SingletonWithInst() = default;

    static T* Instance()
    {
        static T s_Singleton;
        return &s_Singleton;
    }

    /** delete private copy constructor. This is a forbidden operation.*/
    SingletonWithInst(const SingletonWithInst<T>&) = delete;

    /** delete operator= . This is a forbidden operation. */
    SingletonWithInst& operator=(const SingletonWithInst<T>&) = delete;
};