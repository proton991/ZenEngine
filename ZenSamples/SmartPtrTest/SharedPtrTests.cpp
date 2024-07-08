/**
* @file  shared_ptr_test.hpp
* @brief Complete Unit Test of this SharedPtr minimal implementation using Google Test library.
*
* Copyright (c) 2013-2014 Sebastien Rombauts (sebastien.rombauts@gmail.com)
*
* Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
* or copy at http://opensource.org/licenses/MIT)
*/

#include "Common/SharedPtr.h"

#include <vector>

#include <gtest/gtest.h>

using namespace zen;
struct Struct
{
    explicit Struct(int aVal) : mVal(aVal)
    {
        ++_mNbInstances;
    }
    ~Struct(void)
    {
        --_mNbInstances;
    }
    void incr(void)
    {
        ++mVal;
    }
    void decr(void)
    {
        --mVal;
    }

    int mVal;
    static int _mNbInstances;
};

int Struct::_mNbInstances = 0;



TEST(SharedPtr, empty_ptr)
{
    // Create an empty (ie. nullptr) SharedPtr
    SharedPtr<Struct> xPtr;

    EXPECT_EQ(false, xPtr);
    EXPECT_EQ(false, xPtr.Unique());
    EXPECT_EQ(0, xPtr.UseCount());
    EXPECT_EQ((void*)nullptr, xPtr.Get());

    if (xPtr)
    {
        GTEST_FATAL_FAILURE_("bool cast operator error");
    }

    // Reset to nullptr (ie. do nothing)
    xPtr.Reset();

    EXPECT_EQ(false, xPtr);
    EXPECT_EQ(false, xPtr.Unique());
    EXPECT_EQ(0, xPtr.UseCount());
    EXPECT_EQ((void*)nullptr, xPtr.Get());

    // sub-scope
    {
        // Copy construct the empty (ie. nullptr) SharedPtr
        SharedPtr<Struct> yPtr(xPtr);

        EXPECT_EQ(false, xPtr);
        EXPECT_EQ(false, xPtr.Unique());
        EXPECT_EQ(0, xPtr.UseCount());
        EXPECT_EQ((void*)nullptr, xPtr.Get());
        EXPECT_EQ(false, yPtr.Unique());
        EXPECT_EQ(0, yPtr.UseCount());
        EXPECT_EQ((void*)nullptr, yPtr.Get());

        // Assign the empty (ie. nullptr) SharedPtr
        SharedPtr<Struct> zPtr;
        zPtr = xPtr;

        EXPECT_EQ(false, xPtr);
        EXPECT_EQ(false, xPtr.Unique());
        EXPECT_EQ(0, xPtr.UseCount());
        EXPECT_EQ((void*)nullptr, xPtr.Get());
        EXPECT_EQ(false, zPtr.Unique());
        EXPECT_EQ(0, zPtr.UseCount());
        EXPECT_EQ((void*)nullptr, zPtr.Get());
    }
    // end of scope

    EXPECT_EQ(false, xPtr);
    EXPECT_EQ(false, xPtr.Unique());
    EXPECT_EQ(0, xPtr.UseCount());
    EXPECT_EQ((void*)nullptr, xPtr.Get());
}

TEST(SharedPtr, basic_ptr)
{
    {
        // Create a SharedPtr
        SharedPtr<Struct> xPtr(new Struct(123));

        EXPECT_EQ(true, xPtr);
        EXPECT_EQ(true, xPtr.Unique());
        EXPECT_EQ(1, xPtr.UseCount());
        EXPECT_NE((void*)nullptr, xPtr.Get());

        if (xPtr)
        {
            EXPECT_EQ(123, xPtr->mVal);
            EXPECT_EQ(1, xPtr->_mNbInstances);
            EXPECT_EQ(1, Struct::_mNbInstances);

            // call a function
            xPtr->incr();
            EXPECT_EQ(124, xPtr->mVal);
            (*xPtr).incr();
            EXPECT_EQ(125, (*xPtr).mVal);
            xPtr->decr();
            xPtr->decr();

            // Copy construct the SharedPtr
            SharedPtr<Struct> yPtr(xPtr);

            EXPECT_EQ(xPtr, yPtr);
            EXPECT_EQ(true, xPtr);
            EXPECT_EQ(false, xPtr.Unique());
            EXPECT_EQ(2, xPtr.UseCount());
            EXPECT_NE((void*)nullptr, xPtr.Get());
            EXPECT_EQ(123, xPtr->mVal);
            EXPECT_EQ(1, Struct::_mNbInstances);
            EXPECT_EQ(true, yPtr);
            EXPECT_EQ(false, yPtr.Unique());
            EXPECT_EQ(2, yPtr.UseCount());
            EXPECT_NE((void*)nullptr, yPtr.Get());
            EXPECT_EQ(123, yPtr->mVal);
            EXPECT_EQ(1, Struct::_mNbInstances);

            if (yPtr)
            {
                // Assign the SharedPtr
                SharedPtr<Struct> zPtr;
                zPtr = xPtr;

                EXPECT_EQ(xPtr, zPtr);
                EXPECT_EQ(true, xPtr);
                EXPECT_EQ(false, xPtr.Unique());
                EXPECT_EQ(3, xPtr.UseCount());
                EXPECT_NE((void*)nullptr, xPtr.Get());
                EXPECT_EQ(123, xPtr->mVal);
                EXPECT_EQ(1, Struct::_mNbInstances);
                EXPECT_EQ(true, yPtr);
                EXPECT_EQ(false, yPtr.Unique());
                EXPECT_EQ(3, yPtr.UseCount());
                EXPECT_NE((void*)nullptr, yPtr.Get());
                EXPECT_EQ(123, yPtr->mVal);
                EXPECT_EQ(1, Struct::_mNbInstances);
                EXPECT_EQ(true, zPtr);
                EXPECT_EQ(false, zPtr.Unique());
                EXPECT_EQ(3, zPtr.UseCount());
                EXPECT_NE((void*)nullptr, zPtr.Get());
                EXPECT_EQ(123, zPtr->mVal);
                EXPECT_EQ(1, Struct::_mNbInstances);
            }

            EXPECT_EQ(true, xPtr);
            EXPECT_EQ(false, xPtr.Unique());
            EXPECT_EQ(2, xPtr.UseCount());
            EXPECT_NE((void*)nullptr, xPtr.Get());
            EXPECT_EQ(123, xPtr->mVal);
            EXPECT_EQ(1, Struct::_mNbInstances);
        }
        else
        {
            GTEST_FATAL_FAILURE_("bool cast operator error");
        }

        EXPECT_EQ(true, xPtr);
        EXPECT_EQ(true, xPtr.Unique());
        EXPECT_EQ(1, xPtr.UseCount());
        EXPECT_NE((void*)nullptr, xPtr.Get());
        EXPECT_EQ(123, xPtr->mVal);
        EXPECT_EQ(1, Struct::_mNbInstances);
    }

    EXPECT_EQ(0, Struct::_mNbInstances);
}

TEST(SharedPtr, reset_ptr)
{
    // Create an empty (ie. nullptr) SharedPtr
    SharedPtr<Struct> xPtr;

    // Reset it with a new pointer
    xPtr.Reset(new Struct(123));

    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);
    Struct* pX = xPtr.Get();

    // Reset it with another new pointer
    xPtr.Reset(new Struct(234));

    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(234, xPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);
    EXPECT_NE(pX, xPtr.Get());

    // Copy-construct a new SharedPtr to the same object
    SharedPtr<Struct> yPtr(xPtr);

    EXPECT_EQ(xPtr, yPtr);
    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(false, xPtr.Unique());
    EXPECT_EQ(2, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(234, xPtr->mVal);
    EXPECT_EQ(true, yPtr);
    EXPECT_EQ(false, yPtr.Unique());
    EXPECT_EQ(2, yPtr.UseCount());
    EXPECT_NE((void*)nullptr, yPtr.Get());
    EXPECT_EQ(234, yPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);

    // Reset to nullptr
    yPtr.Reset();

    EXPECT_EQ(false, yPtr.Unique());
    EXPECT_EQ(0, yPtr.UseCount());
    EXPECT_EQ((void*)nullptr, yPtr.Get());
    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(234, xPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);

    // Copy-construct a new SharedPtr to the same object
    SharedPtr<Struct> zPtr(xPtr);

    EXPECT_EQ(xPtr, zPtr);
    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(false, xPtr.Unique());
    EXPECT_EQ(2, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(234, xPtr->mVal);
    EXPECT_EQ(true, zPtr);
    EXPECT_EQ(false, zPtr.Unique());
    EXPECT_EQ(2, zPtr.UseCount());
    EXPECT_NE((void*)nullptr, zPtr.Get());
    EXPECT_EQ(234, zPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);

    // Reset it with another new pointer : now xPtr and yPtr each manage a different instance
    zPtr.Reset(new Struct(345));

    EXPECT_NE(xPtr, zPtr);
    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(234, xPtr->mVal);
    EXPECT_EQ(true, zPtr);
    EXPECT_EQ(true, zPtr.Unique());
    EXPECT_EQ(1, zPtr.UseCount());
    EXPECT_NE((void*)nullptr, zPtr.Get());
    EXPECT_EQ(345, zPtr->mVal);
    EXPECT_EQ(2, Struct::_mNbInstances);

    // Reset to nullptr
    zPtr.Reset();

    EXPECT_EQ(false, zPtr.Unique());
    EXPECT_EQ(0, zPtr.UseCount());
    EXPECT_EQ((void*)nullptr, zPtr.Get());
    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(234, xPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);

    // Reset to nullptr
    xPtr.Reset();

    EXPECT_EQ(false, xPtr.Unique());
    EXPECT_EQ(0, xPtr.UseCount());
    EXPECT_EQ((void*)nullptr, xPtr.Get());
    EXPECT_EQ(0, Struct::_mNbInstances);
}

TEST(SharedPtr, compare_ptr)
{
    // Create a SharedPtr
    SharedPtr<Struct> xPtr(new Struct(123));

    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);
    Struct* pX = xPtr.Get();

    // Create another SharedPtr
    SharedPtr<Struct> yPtr(new Struct(234));

    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(2, Struct::_mNbInstances);

    EXPECT_EQ(true, yPtr);
    EXPECT_EQ(true, yPtr.Unique());
    EXPECT_EQ(1, yPtr.UseCount());
    EXPECT_NE((void*)nullptr, yPtr.Get());
    EXPECT_EQ(234, yPtr->mVal);
    Struct* pY = yPtr.Get();

    EXPECT_NE(xPtr, yPtr);
    if (pX < pY)
    {
        EXPECT_LT(xPtr, yPtr);
        EXPECT_LE(xPtr, yPtr);
        EXPECT_GT(yPtr, xPtr);
        EXPECT_GE(yPtr, xPtr);
    }
    else // (pX > pY)
    {
        EXPECT_GT(xPtr, yPtr);
        EXPECT_GE(xPtr, yPtr);
        EXPECT_LT(yPtr, xPtr);
        EXPECT_LE(yPtr, xPtr);
    }

    // Copy a SharedPtr
    SharedPtr<Struct> zPtr = xPtr;
    Struct* pZ             = zPtr.Get();

    EXPECT_EQ(pX, pZ);
    EXPECT_EQ(xPtr, zPtr);
    EXPECT_EQ(zPtr, xPtr);
    EXPECT_GE(xPtr, zPtr);
    EXPECT_LE(xPtr, zPtr);
}

TEST(SharedPtr, std_container)
{
    // Create a SharedPtr
    SharedPtr<Struct> xPtr(new Struct(123));

    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);
    Struct* pX = xPtr.Get();

    {
        std::vector<SharedPtr<Struct>> PtrList;

        // Copy-it inside a container
        PtrList.push_back(xPtr);

        EXPECT_EQ(true, xPtr);
        EXPECT_EQ(false, xPtr.Unique());
        EXPECT_EQ(2, xPtr.UseCount());
        EXPECT_EQ(2, PtrList.back().UseCount());
        EXPECT_EQ(xPtr, PtrList.back());
        EXPECT_EQ(pX, PtrList.back().Get());
        EXPECT_EQ(1, Struct::_mNbInstances);

        // And copy-it again
        PtrList.push_back(xPtr);

        EXPECT_EQ(true, xPtr);
        EXPECT_EQ(false, xPtr.Unique());
        EXPECT_EQ(3, xPtr.UseCount());
        EXPECT_EQ(3, PtrList.back().UseCount());
        EXPECT_EQ(xPtr, PtrList.back());
        EXPECT_EQ(pX, PtrList.back().Get());
        EXPECT_EQ(1, Struct::_mNbInstances);

        // Remove the second pointer from the vector
        PtrList.pop_back();

        EXPECT_EQ(true, xPtr);
        EXPECT_EQ(false, xPtr.Unique());
        EXPECT_EQ(2, xPtr.UseCount());
        EXPECT_EQ(2, PtrList.back().UseCount());
        EXPECT_EQ(xPtr, PtrList.back());
        EXPECT_EQ(pX, PtrList.back().Get());
        EXPECT_EQ(1, Struct::_mNbInstances);

        // Reset the original pointer, leaving 1 last pointer in the vector
        xPtr.Reset();

        EXPECT_EQ(false, xPtr);
        EXPECT_EQ(1, PtrList.back().UseCount());
        EXPECT_EQ(pX, PtrList.back().Get());
        EXPECT_EQ(1, Struct::_mNbInstances);

    } // Destructor of the vector releases the last pointer thus destroying the object

    EXPECT_EQ(0, Struct::_mNbInstances);
}

TEST(SharedPtr, swap_ptr)
{
    // Create a SharedPtr
    SharedPtr<Struct> xPtr(new Struct(123));

    EXPECT_EQ(true, xPtr);
    EXPECT_EQ(true, xPtr.Unique());
    EXPECT_EQ(1, xPtr.UseCount());
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(1, Struct::_mNbInstances);
    Struct* pX = xPtr.Get();

    // Create another SharedPtr
    SharedPtr<Struct> yPtr(new Struct(234));

    EXPECT_EQ(true, yPtr);
    EXPECT_EQ(true, yPtr.Unique());
    EXPECT_EQ(1, yPtr.UseCount());
    EXPECT_NE((void*)nullptr, yPtr.Get());
    EXPECT_EQ(234, yPtr->mVal);
    EXPECT_EQ(2, Struct::_mNbInstances);
    Struct* pY = yPtr.Get();

    if (pX < pY)
    {
        EXPECT_LT(xPtr, yPtr);
        xPtr.Swap(yPtr);
        EXPECT_GT(xPtr, yPtr);
        EXPECT_EQ(true, xPtr);
        EXPECT_EQ(true, yPtr);
    }
    else // (pX > pY)
    {
        EXPECT_GT(xPtr, yPtr);
        xPtr.Swap(yPtr);
        EXPECT_LT(xPtr, yPtr);
        EXPECT_EQ(true, xPtr);
        EXPECT_EQ(true, yPtr);
    }
}

TEST(SharedPtr, assignment_operator)
{
    SharedPtr<Struct> xPtr(new Struct(123));
    SharedPtr<Struct> yPtr;

    yPtr = xPtr;
    EXPECT_EQ(yPtr.UseCount(), 2);
    EXPECT_EQ(xPtr.UseCount(), 2);

    SharedPtr<Struct> zPtr;
    zPtr = std::move(xPtr);
    EXPECT_EQ(zPtr.UseCount(), 2);
    EXPECT_EQ(zPtr->mVal, 123);
}

TEST(SharedPtr, move_constructor)
{
    SharedPtr<Struct> xPtr(new Struct(123));
    SharedPtr<Struct> yPtr = std::move(xPtr);
    EXPECT_EQ(yPtr.UseCount(), 1);
}

class A
{
public:
    A()
    {
        ++_mNbInstances;
    };
    virtual ~A()
    {
        --_mNbInstances;
    };
    static int _mNbInstances;
};
int A::_mNbInstances = 0;

class B : public A
{
public:
    B()
    {
        ++_mNbInstances;
    };
    virtual ~B()
    {
        --_mNbInstances;
    };
    static int _mNbInstances;
};
int B::_mNbInstances = 0;



TEST(SharedPtr, pointer_conv)
{
    SharedPtr<A> a0Ptr;
    EXPECT_EQ(false, a0Ptr);

    {
        SharedPtr<B> bPtr(new B);
        EXPECT_EQ(true, bPtr);
        EXPECT_EQ(true, bPtr.Unique());
        EXPECT_EQ(1, bPtr.UseCount());
        EXPECT_NE((void*)nullptr, bPtr.Get());
        EXPECT_EQ(1, A::_mNbInstances);
        EXPECT_EQ(1, B::_mNbInstances);

        // copy with conversion
        SharedPtr<A> aPtr = bPtr;
        EXPECT_EQ(true, aPtr);
        EXPECT_EQ(false, aPtr.Unique());
        EXPECT_EQ(2, aPtr.UseCount());
        EXPECT_NE((void*)nullptr, aPtr.Get());
        EXPECT_EQ(1, A::_mNbInstances);
        EXPECT_EQ(1, B::_mNbInstances);

        // assignment with conversion
        a0Ptr = bPtr;
        EXPECT_EQ(3, a0Ptr.UseCount());
    }
    // after release of the aPtr and bPtr : only bPtr converted to a0Ptr survived
    EXPECT_EQ(true, a0Ptr);
    EXPECT_EQ(true, a0Ptr.Unique());
    EXPECT_EQ(1, a0Ptr.UseCount());
    EXPECT_NE((void*)nullptr, a0Ptr.Get());
    EXPECT_EQ(1, A::_mNbInstances);
    EXPECT_EQ(1, B::_mNbInstances);

    // release the last one
    a0Ptr.Reset();
    EXPECT_EQ(false, a0Ptr);
    EXPECT_EQ(false, a0Ptr.Unique());
    EXPECT_EQ(0, a0Ptr.UseCount());
    EXPECT_EQ((void*)nullptr, a0Ptr.Get());
    EXPECT_EQ(0, A::_mNbInstances);
    EXPECT_EQ(0, B::_mNbInstances);
}

TEST(SharedPtr, stat_pointer_cast)
{
    SharedPtr<A> a0Ptr;
    EXPECT_EQ(false, a0Ptr);

    {
        SharedPtr<A> aPtr(new A);
        EXPECT_EQ(true, aPtr);
        EXPECT_EQ(true, aPtr.Unique());
        EXPECT_EQ(1, aPtr.UseCount());
        EXPECT_NE((void*)nullptr, aPtr.Get());
        EXPECT_EQ(1, A::_mNbInstances);

        SharedPtr<A> abPtr(new B);
        EXPECT_EQ(true, abPtr);
        EXPECT_EQ(true, abPtr.Unique());
        EXPECT_EQ(1, abPtr.UseCount());
        EXPECT_NE((void*)nullptr, abPtr.Get());
        EXPECT_EQ(2, A::_mNbInstances);
        EXPECT_EQ(1, B::_mNbInstances);

        SharedPtr<B> bPtr(new B);
        EXPECT_EQ(true, bPtr);
        EXPECT_EQ(true, bPtr.Unique());
        EXPECT_EQ(1, bPtr.UseCount());
        EXPECT_NE((void*)nullptr, bPtr.Get());
        EXPECT_EQ(3, A::_mNbInstances);
        EXPECT_EQ(2, B::_mNbInstances);

        // static cast
        SharedPtr<A> a2Ptr = static_pointer_cast<A>(bPtr);
        EXPECT_EQ(true, a2Ptr);
        EXPECT_EQ(false, a2Ptr.Unique());
        EXPECT_EQ(2, a2Ptr.UseCount());
        EXPECT_NE((void*)nullptr, a2Ptr.Get());
        EXPECT_EQ(3, A::_mNbInstances);
        EXPECT_EQ(2, B::_mNbInstances);

        // memorize a2Ptr
        a0Ptr = a2Ptr;
        EXPECT_EQ(3, a0Ptr.UseCount());
    }
    // after release of the aPtr and bPtr : only abPtr converted to a2Ptr survived through a0Ptr
    EXPECT_EQ(true, a0Ptr);
    EXPECT_EQ(true, a0Ptr.Unique());
    EXPECT_EQ(1, a0Ptr.UseCount());
    EXPECT_NE((void*)nullptr, a0Ptr.Get());
    EXPECT_EQ(1, A::_mNbInstances);
    EXPECT_EQ(1, B::_mNbInstances);

    // release the last one
    a0Ptr.Reset();
    EXPECT_EQ(false, a0Ptr);
    EXPECT_EQ(false, a0Ptr.Unique());
    EXPECT_EQ(0, a0Ptr.UseCount());
    EXPECT_EQ((void*)nullptr, a0Ptr.Get());
    EXPECT_EQ(0, A::_mNbInstances);
    EXPECT_EQ(0, B::_mNbInstances);
}

TEST(SharedPtr, dyn_pointer_cast)
{
    SharedPtr<A> a0Ptr;
    EXPECT_EQ(false, a0Ptr);

    {
        SharedPtr<A> aPtr(new A);
        EXPECT_EQ(true, aPtr);
        EXPECT_EQ(true, aPtr.Unique());
        EXPECT_EQ(1, aPtr.UseCount());
        EXPECT_NE((void*)nullptr, aPtr.Get());
        EXPECT_EQ(1, A::_mNbInstances);

        SharedPtr<A> abPtr(new B);
        EXPECT_EQ(true, abPtr);
        EXPECT_EQ(true, abPtr.Unique());
        EXPECT_EQ(1, abPtr.UseCount());
        EXPECT_NE((void*)nullptr, abPtr.Get());
        EXPECT_EQ(2, A::_mNbInstances);
        EXPECT_EQ(1, B::_mNbInstances);

        SharedPtr<B> bPtr(new B);
        EXPECT_EQ(true, bPtr);
        EXPECT_EQ(true, bPtr.Unique());
        EXPECT_EQ(1, bPtr.UseCount());
        EXPECT_NE((void*)nullptr, bPtr.Get());
        EXPECT_EQ(3, A::_mNbInstances);
        EXPECT_EQ(2, B::_mNbInstances);

        // dynamic cast
        SharedPtr<A> a2Ptr = dynamic_pointer_cast<A>(bPtr);
        EXPECT_EQ(true, a2Ptr);
        EXPECT_EQ(false, a2Ptr.Unique());
        EXPECT_EQ(2, a2Ptr.UseCount());
        EXPECT_NE((void*)nullptr, a2Ptr.Get());
        EXPECT_EQ(3, A::_mNbInstances);
        EXPECT_EQ(2, B::_mNbInstances);

        // memorize a2Ptr
        a0Ptr = a2Ptr;
        EXPECT_EQ(3, a0Ptr.UseCount());
    }
    // after release of the aPtr and bPtr : only abPtr converted to a2Ptr survived through a0Ptr
    EXPECT_EQ(true, a0Ptr);
    EXPECT_EQ(true, a0Ptr.Unique());
    EXPECT_EQ(1, a0Ptr.UseCount());
    EXPECT_NE((void*)nullptr, a0Ptr.Get());
    EXPECT_EQ(1, A::_mNbInstances);
    EXPECT_EQ(1, B::_mNbInstances);

    // release the last one
    a0Ptr.Reset();
    EXPECT_EQ(false, a0Ptr);
    EXPECT_EQ(false, a0Ptr.Unique());
    EXPECT_EQ(0, a0Ptr.UseCount());
    EXPECT_EQ((void*)nullptr, a0Ptr.Get());
    EXPECT_EQ(0, A::_mNbInstances);
    EXPECT_EQ(0, B::_mNbInstances);
}
