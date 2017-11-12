#ifndef __MVME_UTIL_STRINGS_H__
#define __MVME_UTIL_STRINGS_H__

#include <QString>

enum class UnitScaling
{
    Binary,
    Decimal,
};

QString format_number(double value, const QString &unit,  UnitScaling scaling,
                      int fieldWidth = 0, char format = 'g', int precision = -1,
                      QChar fillChar = QLatin1Char(' ')
                     );

#endif /* __MVME_UTIL_STRINGS_H__ */
