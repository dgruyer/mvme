#ifndef __MVME_VME_SCRIPT_VARIABLES_H__
#define __MVME_VME_SCRIPT_VARIABLES_H__

#include <QMap>
#include <QString>
#include <QVector>

#include "libmvme_core_export.h"


namespace vme_script
{

struct LIBMVME_CORE_EXPORT Variable
{
    // The variables value. No special handling is done. Variable expansion
    // means simple text replacement.
    QString value;

    // Free form string containing information about where the variable was
    // defined. Could simply be a line number.
    QString definitionLocation;

    Variable()
    {}

    // Constructor taking the variable value and an optional definition
    // location string.
    Variable(const QString &v, const QString &definitionLocation_ = {})
        : value(v)
        , definitionLocation(definitionLocation_)
    { }

    // This constructor takes a lineNumber, converts it to a string and uses it
    // as the definition location.
    Variable(const QString &v, int lineNumber)
        : Variable(v, QString::number(lineNumber))
    { }

    // Variables with a null (default constructed) value are considered invalid.
    // Empty values and non-empty values are considered valid.
    explicit operator bool() const { return !value.isNull(); }
};

struct SymbolTable
{
    QString name;
    QMap<QString, Variable> symbols;

    bool contains(const QString &varName) const
    {
        return symbols.contains(varName);
    }

    Variable value(const QString &varName) const
    {
        return symbols.value(varName);
    }

    bool isEmpty() const
    {
        return symbols.isEmpty();
    }

    Variable &operator[](const QString &varName)
    {
        return symbols[varName];
    }

    const Variable operator[](const QString &varName) const
    {
        return symbols[varName];
    }
};

// Vector of SymbolTables. The first table in the vector is the innermost
// scope and is written to by the 'set' command.
using SymbolTables = QVector<SymbolTable>;

// Lookup a variable in a list of symbol tables.
// Visits symbol tables in order and returns the first Variable stored under
// varName.
LIBMVME_CORE_EXPORT Variable lookup_variable(const QString &varName, const SymbolTables &symtabs);

} // end namespace vme_script

#endif /* __MVME_VME_SCRIPT_VARIABLES_H__ */
