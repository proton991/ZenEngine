/**
* @file  unique_ptr_test.hpp
* @brief Complete Unit Test of this UniquePtr minimal implementation using Google Test library.
*
* Copyright (c) 2014 Sebastien Rombauts (sebastien.rombauts@gmail.com)
*
* Distributed under the MIT License (MIT) (See accompanying file LICENSE.txt
* or copy at http://opensource.org/licenses/MIT)
*/

#include "Common/UniquePtr.h"

#include <vector>

#include <gtest/gtest.h>

using namespace zen;
struct Struct2
{
    explicit Struct2(int aVal) : mVal(aVal)
    {
        ++_mNbInstances;
    }
    ~Struct2(void)
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

int Struct2::_mNbInstances = 0;



TEST(UniquePtr, empty_ptr)
{
    // Create an empty (ie. nullptr) UniquePtr
    UniquePtr<Struct2> xPtr;

    EXPECT_EQ(false, xPtr);
    EXPECT_EQ((void*)nullptr, xPtr.Get());

    if (xPtr)
    {
        GTEST_FATAL_FAILURE_("bool cast operator error");
    }

    // Reset to nullptr (ie. do nothing)
    xPtr.Reset();

    EXPECT_EQ(false, xPtr);
    EXPECT_EQ((void*)nullptr, xPtr.Get());

    // sub-scope
    {
        // Copy construct the empty (ie. nullptr) UniquePtr
        UniquePtr<Struct2> yPtr(xPtr);

        EXPECT_EQ(false, xPtr);
        EXPECT_EQ((void*)nullptr, xPtr.Get());
        EXPECT_EQ((void*)nullptr, yPtr.Get());

        // Assign the empty (ie. nullptr) UniquePtr
        UniquePtr<Struct2> zPtr;
        zPtr = xPtr;

        EXPECT_EQ(false, xPtr);
        EXPECT_EQ((void*)nullptr, xPtr.Get());
        EXPECT_EQ((void*)nullptr, zPtr.Get());
    }
    // end of scope

    EXPECT_EQ(false, xPtr);
    EXPECT_EQ((void*)nullptr, xPtr.Get());
}

TEST(UniquePtr, basic_ptr)
{
    {
        // Create a UniquePtr
        UniquePtr<Struct2> xPtr(new Struct2(123));

        EXPECT_EQ(true, xPtr);
        EXPECT_NE((void*)nullptr, xPtr.Get());

        if (xPtr)
        {
            EXPECT_EQ(123, xPtr->mVal);
            EXPECT_EQ(1, xPtr->_mNbInstances);
            EXPECT_EQ(1, Struct2::_mNbInstances);

            // call a function
            xPtr->incr();
            EXPECT_EQ(124, xPtr->mVal);
            (*xPtr).incr();
            EXPECT_EQ(125, (*xPtr).mVal);
            xPtr->decr();
            xPtr->decr();

            // Copy construct the UniquePtr, transferring ownership
            UniquePtr<Struct2> yPtr(xPtr);

            EXPECT_NE(xPtr, yPtr);
            EXPECT_EQ(false, xPtr);
            EXPECT_EQ((void*)nullptr, xPtr.Get());
            EXPECT_EQ(true, yPtr);
            EXPECT_NE((void*)nullptr, yPtr.Get());
            EXPECT_EQ(123, yPtr->mVal);
            EXPECT_EQ(1, Struct2::_mNbInstances);

            if (yPtr)
            {
                UniquePtr<Struct2> zPtr;
                // Assign the UniquePtr, transferring ownership
                zPtr = yPtr;

                EXPECT_NE(yPtr, zPtr);
                EXPECT_EQ(false, yPtr);
                EXPECT_EQ((void*)nullptr, yPtr.Get());
                EXPECT_EQ(true, zPtr);
                EXPECT_NE((void*)nullptr, zPtr.Get());
                EXPECT_EQ(123, zPtr->mVal);
                EXPECT_EQ(1, Struct2::_mNbInstances);
            }

            EXPECT_EQ(false, xPtr);
            EXPECT_EQ((void*)nullptr, xPtr.Get());
            EXPECT_EQ(false, yPtr);
            EXPECT_EQ((void*)nullptr, yPtr.Get());
            EXPECT_EQ(0, Struct2::_mNbInstances);
        }
        else
        {
            GTEST_FATAL_FAILURE_("bool cast operator error");
        }

        EXPECT_EQ(false, xPtr);
        EXPECT_EQ((void*)nullptr, xPtr.Get());
        EXPECT_EQ(0, Struct2::_mNbInstances);
    }

    EXPECT_EQ(0, Struct2::_mNbInstances);
}

TEST(UniquePtr, reset_ptr)
{
    // Create an empty (ie. nullptr) UniquePtr
    UniquePtr<Struct2> xPtr;

    // Reset it with a new pointer
    xPtr.Reset(new Struct2(123));

    EXPECT_EQ(true, xPtr);
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(1, Struct2::_mNbInstances);
    Struct2* pX = xPtr.Get();

    // Reset it with another new pointer
    xPtr.Reset(new Struct2(234));

    EXPECT_EQ(true, xPtr);
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(234, xPtr->mVal);
    EXPECT_EQ(1, Struct2::_mNbInstances);
    EXPECT_NE(pX, xPtr.Get());

    // Copy-construct a new UniquePtr to the same object, transferring ownership
    UniquePtr<Struct2> yPtr(xPtr);

    EXPECT_NE(xPtr, yPtr);
    EXPECT_EQ(false, xPtr);
    EXPECT_EQ((void*)nullptr, xPtr.Get());
    EXPECT_EQ(true, yPtr);
    EXPECT_NE((void*)nullptr, yPtr.Get());
    EXPECT_EQ(234, yPtr->mVal);
    EXPECT_EQ(1, Struct2::_mNbInstances);

    // Reset to nullptr
    yPtr.Reset();

    EXPECT_EQ((void*)nullptr, yPtr.Get());
    EXPECT_EQ(false, xPtr);
    EXPECT_EQ((void*)nullptr, xPtr.Get());
    EXPECT_EQ(0, Struct2::_mNbInstances);
}

TEST(UniquePtr, compare_ptr)
{
    // Create a UniquePtr
    UniquePtr<Struct2> xPtr(new Struct2(123));

    EXPECT_EQ(true, xPtr);
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(1, Struct2::_mNbInstances);
    Struct2* pX = xPtr.Get();

    // Create another UniquePtr
    UniquePtr<Struct2> yPtr(new Struct2(234));

    EXPECT_EQ(true, xPtr);
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(2, Struct2::_mNbInstances);

    EXPECT_EQ(true, yPtr);
    EXPECT_NE((void*)nullptr, yPtr.Get());
    EXPECT_EQ(234, yPtr->mVal);
    Struct2* pY = yPtr.Get();

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
}

TEST(UniquePtr, swap_ptr)
{
    // Create a UniquePtr
    UniquePtr<Struct2> xPtr(new Struct2(123));

    EXPECT_EQ(true, xPtr);
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(1, Struct2::_mNbInstances);
    Struct2* pX = xPtr.Get();

    // Create another UniquePtr
    UniquePtr<Struct2> yPtr(new Struct2(234));

    EXPECT_EQ(true, yPtr);
    EXPECT_NE((void*)nullptr, yPtr.Get());
    EXPECT_EQ(234, yPtr->mVal);
    EXPECT_EQ(2, Struct2::_mNbInstances);
    Struct2* pY = yPtr.Get();

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

TEST(UniquePtr, std_container)
{
    // Create a shared_ptr
    UniquePtr<Struct2> xPtr(new Struct2(123));

    EXPECT_EQ(true, xPtr);
    EXPECT_NE((void*)nullptr, xPtr.Get());
    EXPECT_EQ(123, xPtr->mVal);
    EXPECT_EQ(1, Struct2::_mNbInstances);
    Struct2* pX = xPtr.Get();

    {
        std::vector<UniquePtr<Struct2>> PtrList;

        // Move-it inside a container, transferring ownership
        PtrList.push_back(std::move(xPtr));

        EXPECT_EQ(false, xPtr);
        EXPECT_EQ(true, PtrList.back());
        EXPECT_EQ(pX, PtrList.back().Get());
        EXPECT_EQ(1, Struct2::_mNbInstances);

    } // Destructor of the vector releases the last pointer thus destroying the object

    EXPECT_EQ(0, Struct2::_mNbInstances);
}
