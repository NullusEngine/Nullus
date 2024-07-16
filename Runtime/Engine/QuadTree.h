#pragma once
#include "Vector2.h"
#include "Vector3.h"
#include <list>
#include <functional>
#include "EngineDef.h"
namespace NLS
{
namespace Engine
{
template<class T>
class QuadTree;

template<class T>
struct QuadTreeEntry
{
    Maths::Vector3 pos;
    Maths::Vector3 size;
    T object;

    QuadTreeEntry(T obj, Maths::Vector3 pos, Maths::Vector3 size)
    {
        object = obj;
        this->pos = pos;
        this->size = size;
    }
};
static bool AABBTest(const Maths::Vector3& posA, const Maths::Vector3& posB, const Maths::Vector3& halfSizeA, const Maths::Vector3& halfSizeB)
{
    Maths::Vector3 delta = posB - posA;
    Maths::Vector3 totalSize = halfSizeA + halfSizeB;

    if (abs(delta.x) < totalSize.x && abs(delta.y) < totalSize.y && abs(delta.z) < totalSize.z)
    {
        return true;
    }
    return false;
}
template<class T>
class QuadTreeNode
{
public:
    typedef std::function<void(std::list<QuadTreeEntry<T>>&)> QuadTreeFunc;

    void Insert(T& object, const Maths::Vector3& objectPos, const Maths::Vector3& objectSize, int depthLeft, int maxSize)
    {
        if (!AABBTest(objectPos, Maths::Vector3(position.x, 0, position.y), objectSize, Maths::Vector3(size.x, 1000.0f, size.y)))
        {
            return;
        }
        if (children)
        { // not a leaf node , just descend the tree
            for (int i = 0; i < 4; ++i)
            {
                children[i].Insert(object, objectPos, objectSize, depthLeft - 1, maxSize);
            }
        }
        else // currently a leaf node , can just expand
        {
            contents.push_back(QuadTreeEntry<T>(object, objectPos, objectSize));
            if ((int)contents.size() > maxSize && depthLeft > 0)
            {
                if (!children)
                {
                    Split();
                    // we need to reinsert the contents so far !
                    for (const auto& i : contents)
                    {
                        for (int j = 0; j < 4; ++j)
                        {
                            auto entry = i;
                            children[j].Insert(entry.object, entry.pos, entry.size, depthLeft - 1, maxSize);
                        }
                    }
                    contents.clear(); // contents now distributed !
                }
            }
        }
    }

protected:
    friend class QuadTree<T>;

    QuadTreeNode() {}

    QuadTreeNode(Maths::Vector2 pos, Maths::Vector2 size)
    {
        children = nullptr;
        this->position = pos;
        this->size = size;
    }

    ~QuadTreeNode()
    {
        delete[] children;
    }



    void Split()
    {
        Maths::Vector2 halfSize = size / 2.0f;
        children = new QuadTreeNode<T>[4];
        children[0] = QuadTreeNode<T>(position +  Maths::Vector2(-halfSize.x, halfSize.y), halfSize);
        children[1] = QuadTreeNode<T>(position +  Maths::Vector2(halfSize.x, halfSize.y), halfSize);
        children[2] = QuadTreeNode<T>(position +  Maths::Vector2(-halfSize.x, -halfSize.y), halfSize);
        children[3] = QuadTreeNode<T>(position +  Maths::Vector2(halfSize.x, -halfSize.y), halfSize);
    }

    void DebugDraw()
    {
    }

    void OperateOnContents(QuadTreeFunc& func)
    {
        if (children)
        {
            for (int i = 0; i < 4; ++i)
            {
                children[i].OperateOnContents(func);
            }
        }
        else
        {
            if (!contents.empty())
            {
                func(contents);
            }
        }
    }

protected:
    std::list<QuadTreeEntry<T>> contents;

    Maths::Vector2 position;
    Maths::Vector2 size;

    QuadTreeNode<T>* children;
};
} // namespace Engine
} // namespace NLS


namespace NLS
{
namespace Engine
{
template<class T>
class QuadTree
{
public:
    QuadTree(Maths::Vector2 size, int maxDepth = 6, int maxSize = 5)
    {
        root = QuadTreeNode<T>(Maths::Vector2(), size);
        this->maxDepth = maxDepth;
        this->maxSize = maxSize;
    }
    ~QuadTree()
    {
    }

    void Insert(T object, const Maths::Vector3& pos, const Maths::Vector3& size)
    {
        root.Insert(object, pos, size, maxDepth, maxSize);
    }

    void DebugDraw()
    {
        root.DebugDraw();
    }

    void OperateOnContents(typename QuadTreeNode<T>::QuadTreeFunc func)
    {
        root.OperateOnContents(func);
    }

protected:
    QuadTreeNode<T> root;
    int maxDepth;
    int maxSize;
};
} // namespace Engine
} // namespace NLS