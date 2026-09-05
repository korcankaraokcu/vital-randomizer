#include "VitalState.h"

namespace vitalstate
{
    namespace
    {
        constexpr const char* kMagic = "VC2!";

        // Offsets into the decoded VstW / FXB component block. These are fixed by
        // the format: an FXB "bank with chunk" header is always 176 bytes before
        // the payload starts.
        constexpr size_t kFxbByteSizeOffset = 20;   // big endian, total - 24
        constexpr size_t kChunkSizeOffset   = 172;  // big endian, total - 176

        juce::String extractTag (const juce::String& xml, const juce::String& tag)
        {
            const auto open = "<" + tag + ">";
            const auto close = "</" + tag + ">";
            const auto a = xml.indexOf (open);
            if (a < 0)
                return {};
            const auto start = a + open.length();
            const auto b = xml.indexOf (start, close);
            if (b < 0)
                return {};
            return xml.substring (start, b).trim();
        }

        void writeBigEndian32 (juce::MemoryBlock& block, size_t offset, uint32_t value)
        {
            if (offset + 4 > block.getSize())
                return;
            auto* p = static_cast<uint8_t*> (block.getData()) + offset;
            p[0] = static_cast<uint8_t> ((value >> 24) & 0xff);
            p[1] = static_cast<uint8_t> ((value >> 16) & 0xff);
            p[2] = static_cast<uint8_t> ((value >> 8) & 0xff);
            p[3] = static_cast<uint8_t> (value & 0xff);
        }

        /*  Find where the JSON object starting at `start` ends.

            A brace counter is enough here, but only if it skips over string
            contents. Preset names and comments are user supplied and routinely
            contain braces, so counting them would truncate the patch.
        */
        size_t findJsonEnd (const char* data, size_t start, size_t size)
        {
            int depth = 0;
            bool inString = false, escaped = false;

            for (size_t i = start; i < size; ++i)
            {
                const char c = data[i];

                if (inString)
                {
                    if (escaped)          escaped = false;
                    else if (c == '\\')   escaped = true;
                    else if (c == '"')    inString = false;
                    continue;
                }

                if (c == '"')            inString = true;
                else if (c == '{')       ++depth;
                else if (c == '}' && --depth == 0)
                    return i + 1;
            }
            return 0;
        }

        struct Split
        {
            bool ok = false;
            juce::MemoryBlock header;      // FXB block up to the JSON
            juce::MemoryBlock trailing;    // JUCE private data after the JSON
            juce::String editController;   // carried through untouched
            size_t jsonStart = 0, jsonEnd = 0;
            juce::MemoryBlock component;
        };

        Split split (const juce::MemoryBlock& state)
        {
            Split s;
            if (state.getSize() < 16 || std::memcmp (state.getData(), kMagic, 4) != 0)
                return s;

            const juce::String xml (juce::CharPointer_UTF8 (
                                        static_cast<const char*> (state.getData()) + 8),
                                    state.getSize() - 8);

            const auto componentB64 = extractTag (xml, "IComponent");
            if (componentB64.isEmpty())
                return s;

            if (! s.component.fromBase64Encoding (componentB64))
                return s;

            s.editController = extractTag (xml, "IEditController");

            const auto* data = static_cast<const char*> (s.component.getData());
            const auto size = s.component.getSize();

            size_t start = 0;
            for (size_t i = 0; i + 1 < size; ++i)
            {
                if (data[i] == '{' && data[i + 1] == '"')
                {
                    start = i;
                    break;
                }
            }
            if (start == 0)
                return s;

            const auto end = findJsonEnd (data, start, size);
            if (end == 0)
                return s;

            s.jsonStart = start;
            s.jsonEnd = end;
            s.header.append (data, start);
            s.trailing.append (data + end, size - end);
            s.ok = true;
            return s;
        }
    }

    bool looksValid (const juce::MemoryBlock& state)
    {
        return split (state).ok;
    }

    nlohmann::json read (const juce::MemoryBlock& state)
    {
        const auto s = split (state);
        if (! s.ok)
            return {};

        const auto* data = static_cast<const char*> (s.component.getData());
        return nlohmann::json::parse (data + s.jsonStart, data + s.jsonEnd, nullptr, false);
    }

    juce::MemoryBlock write (const juce::MemoryBlock& templateState,
                             const nlohmann::json& preset)
    {
        const auto s = split (templateState);
        if (! s.ok)
            return {};

        const auto body = preset.dump();

        juce::MemoryBlock component;
        component.append (s.header.getData(), s.header.getSize());
        component.append (body.data(), body.size());
        component.append (s.trailing.getData(), s.trailing.getSize());

        const auto total = static_cast<uint32_t> (component.getSize());
        writeBigEndian32 (component, kChunkSizeOffset, total - 176);
        writeBigEndian32 (component, kFxbByteSizeOffset, total - 24);

        juce::String xml;
        xml << "<?xml version=\"1.0\" encoding=\"UTF-8\"?> <VST3PluginState><IComponent>"
            << component.toBase64Encoding()
            << "</IComponent>";
        if (s.editController.isNotEmpty())
            xml << "<IEditController>" << s.editController << "</IEditController>";
        xml << "</VST3PluginState>";

        const auto utf8 = xml.toRawUTF8();
        const auto xmlBytes = std::strlen (utf8);

        juce::MemoryBlock out;
        out.append (kMagic, 4);

        // Little endian here, unlike the FXB fields above. The VC2! container is
        // JUCE's own and the FXB block inside it is Steinberg's, so the two
        // disagree about byte order within the same file.
        const uint32_t length = static_cast<uint32_t> (xmlBytes + 1);
        const uint8_t le[4] = { static_cast<uint8_t> (length & 0xff),
                                static_cast<uint8_t> ((length >> 8) & 0xff),
                                static_cast<uint8_t> ((length >> 16) & 0xff),
                                static_cast<uint8_t> ((length >> 24) & 0xff) };
        out.append (le, 4);
        out.append (utf8, xmlBytes);
        const uint8_t nul = 0;
        out.append (&nul, 1);
        return out;
    }
}
