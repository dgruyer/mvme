/* mvme - Mesytec VME Data Acquisition
 *
 * Copyright (C) 2016, 2017  Florian Lüke <f.lueke@mesytec.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */
#ifndef UTIL_H
#define UTIL_H

#include "typedefs.h"
#include "qt_util.h"

#include <QMetaType>
#include <QPair>
#include <QVariant>
#include <QVector>
#include <QWidget>

#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>

#define ArrayCount(a) (sizeof(a) / sizeof((a)[0]))

Q_DECLARE_SMART_POINTER_METATYPE(std::shared_ptr);

class QTextStream;

void debugOutputBuffer(u32 *dataBuffer, u32 bufferCount);

QVector<u32> parseStackFile(QTextStream &input);
QVector<u32> parseStackFile(const QString &input);

typedef QPair<u32, QVariant> RegisterSetting; // (addr, value)
typedef QVector<RegisterSetting> RegisterList;

RegisterList parseRegisterList(QTextStream &input, u32 baseAddress = 0);
RegisterList parseRegisterList(const QString &input, u32 baseAddress = 0);

inline bool isFloat(const QVariant &var)
{
    return (static_cast<QMetaType::Type>(var.type()) == QMetaType::Float);
}

QString toString(const RegisterList &registerList);
QStringList toStringList(const RegisterList &registerList);

class end_of_buffer: public std::exception {};

struct BufferIterator
{
    enum Alignment { Align16, Align32 };

    BufferIterator(u8 *data, size_t size, Alignment alignment = Align32)
        : data(data)
        , buffp(data)
        , endp(data + size)
        , size(size)
        , alignment(alignment)
    {}

    u8 *data;
    u8 *buffp;
    u8 *endp;
    size_t size;
    Alignment alignment;

    inline bool align32() const { return alignment == Align32; }

    inline u8 extractU8()
    {
        if (buffp + sizeof(u8) > endp)
            throw end_of_buffer();

        u8 ret = *buffp;
        buffp += sizeof(u8);
        return ret;
    }

    inline u16 extractU16()
    {
        if (buffp + sizeof(u16) > endp)
            throw end_of_buffer();

        u32 ret = *reinterpret_cast<u16 *>(buffp);
        buffp += sizeof(u16);
        return ret;
    }

    inline u32 extractU32()
    {
        if (buffp + sizeof(u32) > endp)
            throw end_of_buffer();

        u32 ret = *reinterpret_cast<u32 *>(buffp);
        buffp += sizeof(u32);
        return ret;
    }

    inline u32 extractWord()
    {
        return align32() ? extractU32() : extractU16();
    }

    inline u8 extractByte()
    {
        return extractU8();
    }

    inline u16 extractShortword()
    {
        return extractU16();
    }

    inline u32 extractLongword()
    {
        return extractU32();
    }

    inline u16 peekU16() const
    {
        if (buffp + sizeof(u16) > endp)
            throw end_of_buffer();

        u32 ret = *reinterpret_cast<u16 *>(buffp);
        return ret;
    }

    inline u32 peekU32() const
    {
        if (buffp + sizeof(u32) > endp)
            throw end_of_buffer();

        u32 ret = *reinterpret_cast<u32 *>(buffp);
        return ret;
    }

    inline u32 peekWord() const
    {
        return align32() ? peekU32() : peekU16();
    }

    inline u32 bytesLeft() const
    {
        return endp - buffp;
    }

    inline u32 wordsLeft() const
    {
        return bytesLeft() / (align32() ? sizeof(u32) : sizeof(u16));
    }

    inline u32 shortwordsLeft() const
    {
        return bytesLeft() / sizeof(u16);
    }

    inline u32 longwordsLeft() const
    {
        return bytesLeft() / sizeof(u32);
    }

    inline u16 *asU16() { return reinterpret_cast<u16 *>(buffp); }
    inline u32 *asU32() { return reinterpret_cast<u32 *>(buffp); }

    inline void skip(size_t bytes)
    {
        buffp += bytes;
        if (buffp > endp)
            buffp = endp;
    }

    inline void skip(size_t width, size_t count)
    {
        skip(width * count);
    }

    inline bool atEnd() const { return buffp == endp; }
};

QString readStringFile(const QString &filename);

template<typename T>
T *Var2Ptr(const QVariant &variant)
{
    return static_cast<T *>(variant.value<void *>());
}

template<typename T>
T *Var2QObject(const QVariant &variant)
{
    return qobject_cast<T *>(Var2Ptr<QObject>(variant));
}

template<typename T>
QVariant Ptr2Var(T *ptr)
{
    return QVariant::fromValue(static_cast<void *>(ptr));
}

QString makeDurationString(qint64 durationSeconds);

/** Emits aboutToClose() before returning from closeEvent() */
class MVMEWidget: public QWidget
{
    Q_OBJECT
    signals:
        void aboutToClose();

    public:
        MVMEWidget(QWidget *parent = 0);

    protected:
        void closeEvent(QCloseEvent *event) override;
};

class TemplateLoader: public QObject
{
    Q_OBJECT
    signals:
        void logMessage(const QString &msg);

    public:
        QString getTemplatePath();
        QString readTemplate(const QString &name);

    private:
        QString m_templatePath;
};

// Source: http://stackoverflow.com/a/757266
inline int trailing_zeroes(uint32_t v)
{
    int r;           // result goes here
    static const int MultiplyDeBruijnBitPosition[32] =
    {
        0, 1, 28, 2, 29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4, 8,
        31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6, 11, 5, 10, 9
    };
    r = MultiplyDeBruijnBitPosition[((uint32_t)((v & -v) * 0x077CB531U)) >> 27];
    return r;
}

// Source: http://stackoverflow.com/a/109025 (SWAR)
inline u32 number_of_set_bits(u32 i)
{
     // Java: use >>> instead of >>
     // C or C++: use uint32_t
     i = i - ((i >> 1) & 0x55555555);
     i = (i & 0x33333333) + ((i >> 2) & 0x33333333);
     return (((i + (i >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;
}

QJsonDocument gui_read_json(QIODevice *input);
QJsonDocument gui_read_json_file(const QString &fileName);
bool gui_write_json_file(const QString &fileName, const QJsonDocument &doc);

QPair<double, QString> byte_unit(size_t bytes);

//QString format_memory_size(size_t bytes);

void logBuffer(BufferIterator iter, std::function<void (const QString &)> loggerFun);

static constexpr double make_quiet_nan()
{
    return std::numeric_limits<double>::quiet_NaN();
}

inline constexpr size_t Kilobytes(size_t x) { return x * 1024; }
inline constexpr size_t Megabytes(size_t x) { return Kilobytes(x) * 1024; }
inline constexpr size_t Gigabytes(size_t x) { return Megabytes(x) * 1024; }

#define InvalidCodePath Q_ASSERT(!"invalid code path")
#define InvalidDefaultCase default: { Q_ASSERT(!"invalid default case"); }

template<typename Code>
struct ReadResultBase
{
    typedef Code CodeType;

    static const QMap<Code, const char *> ErrorCodeStrings;

    Code code;
    QMap<QString, QVariant> errorData;

    QString toRichText() const;

    inline operator bool() const { return code == Code::NoError; }
};


template<typename Code>
QString ReadResultBase<Code>::toRichText() const
{
    QString result;

    if (code != Code::NoError)
    {
        //result += ErrorCodeStrings.value(code, "Unknown error");
        result += QSL("<table>");
        result += QString("<tr><td>Error cause:</td><td>%1</td>")
            .arg(ErrorCodeStrings.value(code, "Unknown error"));

        for (auto it = errorData.begin(); it != errorData.end(); ++it)
        {
            result += QString("<tr><td>%1:</td><td>%2</td></tr>")
                .arg(it.key())
                .arg(it.value().toString());
        }
        result += QSL("</table>");
    }

    return result;
}

#endif // UTIL_H
