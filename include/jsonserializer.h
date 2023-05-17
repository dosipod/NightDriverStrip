//+--------------------------------------------------------------------------
//
// File:        jsonserializer.h
//
// NightDriverStrip - (c) 2018 Plummer's Software LLC.  All Rights Reserved.
//
// This file is part of the NightDriver software project.
//
//    NightDriver is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 3 of the License, or
//    (at your option) any later version.
//
//    NightDriver is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with Nightdriver.  It is normally found in copying.txt
//    If not, see <https://www.gnu.org/licenses/>.
//
// Description:
//
//    Declares classes for JSON (de)serialization of selected
//    properties of selected classes
//
// History:     Mar-29-2023         Rbergen      Created for NightDriverStrip
//
//---------------------------------------------------------------------------

#pragma once

#include <atomic>
#include <ArduinoJson.h>
#include "jsonbase.h"
#include "FastLED.h"

struct IJSONSerializable
{
    virtual bool SerializeToJSON(JsonObject& jsonObject) = 0;
    virtual bool DeserializeFromJSON(const JsonObjectConst& jsonObject) { return false; }
};

template <class E>
constexpr auto to_value(E e) noexcept
{
	return static_cast<std::underlying_type_t<E>>(e);
}

#if USE_PSRAM
    struct JsonPsramAllocator
    {
        void* allocate(size_t size) {
            return ps_malloc(size);
        }

        void deallocate(void* pointer) {
            free(pointer);
        }

        void* reallocate(void* ptr, size_t new_size) {
            return ps_realloc(ptr, new_size);
        }
    };

    typedef BasicJsonDocument<JsonPsramAllocator> AllocatedJsonDocument;

#else
    typedef DynamicJsonDocument AllocatedJsonDocument;
#endif

namespace ArduinoJson
{
    template <>
    struct Converter<CRGB>
    {
        static bool toJson(const CRGB& color, JsonVariant dst)
        {
            return dst.set((uint32_t)((color.r << 16) | (color.g << 8) | color.b));
        }

        static CRGB fromJson(JsonVariantConst src)
        {
            return CRGB(src.as<uint32_t>());
        }

        static bool checkJson(JsonVariantConst src)
        {
            return src.is<uint32_t>();
        }
    };

    template <>
    struct Converter<CRGBPalette16>
    {
        static bool toJson(const CRGBPalette16& palette, JsonVariant dst)
        {
            AllocatedJsonDocument doc(384);

            JsonArray colors = doc.to<JsonArray>();

            for (auto& color: palette.entries)
                colors.add(color);

            return dst.set(doc);
        }

        static CRGBPalette16 fromJson(JsonVariantConst src)
        {
            CRGB colors[16];
            int colorIndex = 0;

            JsonArrayConst componentsArray = src.as<JsonArrayConst>();
            for (JsonVariantConst value : componentsArray)
                colors[colorIndex++] = value.as<CRGB>();

            return CRGBPalette16(colors);
        }

        static bool checkJson(JsonVariantConst src)
        {
            return src.is<JsonArrayConst>() && src.as<JsonArrayConst>().size() == 16;
        }
    };
}

bool LoadJSONFile(const char *fileName, size_t& bufferSize, std::unique_ptr<AllocatedJsonDocument>& pJsonDoc);
bool SaveToJSONFile(const char *fileName, size_t& bufferSize, IJSONSerializable& object);
bool RemoveJSONFile(const char *fileName);

#define JSON_WRITER_DELAY 3000

class JSONWriter
{
  private:

    // Writer function and flag combo
    struct WriterEntry
    {
        std::atomic_bool flag = false;
        std::function<void()> writer;

        WriterEntry(std::function<void()> writer) :
            writer(writer)
        {}

        WriterEntry(WriterEntry&& entry) : WriterEntry(entry.writer)
        {}
    };

    std::vector<WriterEntry> writers;
    std::atomic_ulong latestFlagMs;
    TaskHandle_t writerTask;

    static void WriterInvokerEntryPoint(void * pv)
    {
        JSONWriter * pObj = (JSONWriter *) pv;

        for(;;)
        {
            TickType_t notifyWait = portMAX_DELAY;

            for (;;)
            {
                // Wait until we're woken up by a writer being flagged, or until we've reached the hold point
                ulTaskNotifyTake(pdTRUE, notifyWait);

                unsigned long holdUntil = pObj->latestFlagMs + JSON_WRITER_DELAY;
                unsigned long now = millis();
                if (now >= holdUntil)
                    break;

                notifyWait = pdMS_TO_TICKS(holdUntil - now);
            }

            for (auto &entry : pObj->writers)
            {
                // Unset flag before we do the actual write. This makes that we don't miss another flag raise if it happens while writing
                if (entry.flag.exchange(false))
                    entry.writer();
            }
        }
    }

  public:
    JSONWriter()
    {
        xTaskCreatePinnedToCore(WriterInvokerEntryPoint, "JSONWriter", 4096, (void *) this, JSONSERIAL_PRIORITY, &writerTask, JSONSERIAL_CORE);
    }

    ~JSONWriter()
    {
        vTaskDelete(writerTask);
    }

    // Add a writer to the collection. Returns the index of the added writer, for use with FlagWriter()
    size_t RegisterWriter(std::function<void()> writer)
    {
        // Add the writer with its flag unset
        writers.emplace_back(writer);
        return writers.size() - 1;
    }

    // Flag a writer for invocation and wake up the task that calls them
    void FlagWriter(size_t index)
    {
        // Check if we received a valid writer index
        if (index >= writers.size())
            return;

        writers[index].flag = true;
        latestFlagMs = millis();

        // Wake up the writer invoker task if it's sleeping, or request another write cycle if it isn't
        xTaskNotifyGive(writerTask);
    }
};

extern DRAM_ATTR std::unique_ptr<JSONWriter> g_ptrJSONWriter;