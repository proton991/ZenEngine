#pragma once
#include "AssetLib/ImageAsset.h"
#include "AssetLib/ImportErrors.h"

namespace zen::asset
{
// Shared CPU image decoding. Independent calls may run on processing workers.
// Compressed source memory is borrowed only for the duration of this call.
HeapVector<uint8_t> DecodeImagePixels(const uint8_t*                   data,
                                      size_t                           size,
                                      uint32_t&                        width,
                                      uint32_t&                        height,
                                      ImportErrors&                    errors,
                                      HeapVector<HeapVector<uint8_t>>* mips = nullptr);
} // namespace zen::asset
