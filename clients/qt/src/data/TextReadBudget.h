#pragma once
#include <QtGlobal>
namespace reid::data {
inline bool validTextReadChunk(qint64 expectedCharacters, qint64 consumedCharacters,
        qint64 characters, qint64 bytes, qint64 capacity, qint64 consumedBytes, qint64 limit) {
    return expectedCharacters >= 0 && consumedCharacters >= 0 && consumedCharacters <= expectedCharacters
        && characters > 0 && characters <= expectedCharacters - consumedCharacters
        && bytes > 0 && bytes <= capacity && consumedBytes >= 0 && consumedBytes <= limit
        && bytes <= limit - consumedBytes;
}
inline bool completeTextRead(qint64 expectedCharacters, qint64 consumedCharacters) {
    return expectedCharacters >= 0 && expectedCharacters == consumedCharacters;
}
}
