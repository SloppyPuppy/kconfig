/*
    This file is part of the KDE libraries.

    SPDX-FileCopyrightText: 2003 Cornelius Schumacher <schumacher@kde.org>
    SPDX-FileCopyrightText: 2003 Waldo Bastian <bastian@kde.org>
    SPDX-FileCopyrightText: 2003 Zack Rusin <zack@kde.org>
    SPDX-FileCopyrightText: 2006 Michaël Larouche <michael.larouche@kdemail.net>
    SPDX-FileCopyrightText: 2008 Allen Winter <winter@kde.org>
    SPDX-FileCopyrightText: 2020 Tomaz Cananbrava <tcanabrava@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "KConfigCodeGeneratorBase.h"
#include "KConfigParameters.h"
#include "KConfigCommonStructs.h"

#include <QTextStream>
#include <QLatin1Char>
#include <QFileInfo>

#include <ostream>
#include <QDebug>

#include <iostream>

KConfigCodeGeneratorBase::KConfigCodeGeneratorBase(
    const QString &inputFile,
    const QString &baseDir,
    const QString &fileName,
    const KConfigParameters &parameters,
    ParseResult &parseResult)
    : parseResult(parseResult), m_inputFile(inputFile), m_baseDir(baseDir), m_fileName(fileName), m_cfg(parameters)
{
    m_file.setFileName(m_fileName);
    if (!m_file.open(QIODevice::WriteOnly)) {
        std::cerr << "Can not open '" << qPrintable(m_fileName) << "for writing." << std::endl;
        exit(1);
    }
    m_stream.setDevice(&m_file);
    m_stream.setCodec("utf-8");

    if (m_cfg.staticAccessors) {
        m_this = QStringLiteral("self()->");
    } else {
        m_const = QStringLiteral(" const");
    }
}

KConfigCodeGeneratorBase::~KConfigCodeGeneratorBase()
{
    save();
}

void KConfigCodeGeneratorBase::save()
{
    m_file.close();
}

//TODO: Remove this weird logic and adapt the testcases
void KConfigCodeGeneratorBase::indent()
{
    if (m_indentLevel >= 4) {
        m_indentLevel += 2;
    } else {
        m_indentLevel += 4;
    }
}

void KConfigCodeGeneratorBase::unindent()
{
    if (m_indentLevel > 4) {
        m_indentLevel -= 2;
    } else {
        m_indentLevel -= 4;
    }
}

QString KConfigCodeGeneratorBase::whitespace() const
{
    QString spaces;
    for (int i = 0; i < m_indentLevel; i++) {
        spaces.append(QLatin1Char(' '));
    }
    return spaces;
}

void KConfigCodeGeneratorBase::startScope()
{
    m_stream << whitespace() << QLatin1Char('{');
    m_stream << '\n';
    indent();
}

void KConfigCodeGeneratorBase::endScope(ScopeFinalizer finalizer)
{
    unindent();
    m_stream << whitespace() << QLatin1Char('}');
    if (finalizer == ScopeFinalizer::Semicolon) {
        m_stream << ';';
    }
    m_stream << '\n';
}

void KConfigCodeGeneratorBase::start()
{
    const QString m_fileName = QFileInfo(m_inputFile).fileName();
    m_stream << "// This file is generated by kconfig_compiler_kf5 from " << m_fileName << ".kcfg" << ".\n";
    m_stream << "// All changes you do to this file will be lost.\n";
}

void KConfigCodeGeneratorBase::addHeaders(const QStringList &headerList)
{
    for (auto include : qAsConst(headerList)) {
        if (include.startsWith(QLatin1Char('"'))) {
            m_stream << "#include " << include << '\n';
        } else {
            m_stream << "#include <" << include << ">\n";
        }
    }
}

// adds as many 'namespace foo {' lines to p_out as
// there are namespaces in p_ns
void KConfigCodeGeneratorBase::beginNamespaces()
{
    if (!m_cfg.nameSpace.isEmpty()) {
        for (const QString &ns : m_cfg.nameSpace.split(QStringLiteral("::"))) {
            m_stream << "namespace " << ns << " {\n";
        }
        m_stream << '\n';
    }
}

// adds as many '}' lines to p_out as
// there are namespaces in p_ns
void KConfigCodeGeneratorBase::endNamespaces()
{
    if (!m_cfg.nameSpace.isEmpty()) {
        m_stream << '\n';
        const int namespaceCount = m_cfg.nameSpace.count(QStringLiteral("::")) + 1;
        for (int i = 0; i < namespaceCount; ++i) {
            m_stream << "}\n";
        }
    }
}

// returns the member accesor implementation
// which should go in the h file if inline
// or the cpp file if not inline
QString KConfigCodeGeneratorBase::memberAccessorBody(const CfgEntry *e, bool globalEnums) const
{
    QString result;
    QTextStream out(&result, QIODevice::WriteOnly);
    QString n = e->name;
    QString t = e->type;
    bool useEnumType = m_cfg.useEnumTypes && t == QLatin1String("Enum");

    out << "return ";
    if (useEnumType) {
        out << "static_cast<" << enumType(e, globalEnums) << ">(";
    }
    out << m_this << varPath(n, m_cfg);
    if (!e->param.isEmpty()) {
        out << "[i]";
    }
    if (useEnumType) {
        out << ")";
    }
    out << ";\n";

    return result;
}

void KConfigCodeGeneratorBase::memberImmutableBody(const CfgEntry *e, bool globalEnums)
{
    QString n = e->name;
    QString t = e->type;

    stream() << whitespace() << "return " << m_this << "isImmutable( QStringLiteral( \"";
    if (!e->param.isEmpty()) {
        stream() << QString(e->paramName).replace(QLatin1String("$(") + e->param + QLatin1Char(')'), QLatin1String("%1")) << "\" ).arg( ";
        if (e->paramType == QLatin1String("Enum")) {
            stream() << "QLatin1String( ";

            if (globalEnums) {
                stream() << enumName(e->param) << "ToString[i]";
            } else {
                stream() << enumName(e->param) << "::enumToString[i]";
            }

            stream() << " )";
        } else {
            stream() << "i";
        }
        stream() << " )";
    } else {
        stream() << n << "\" )";
    }
    stream() << " );\n";
}

void KConfigCodeGeneratorBase::createIfSetLogic(const CfgEntry *e, const QString &varExpression)
{
    const QString n = e->name;
    const QString t = e->type;
    const bool hasBody = !e->signalList.empty() || m_cfg.generateProperties;

    m_stream << whitespace() << "if (";
    if (hasBody) {
        m_stream << "v != " << varExpression << " && ";
    }

    const auto immutablefunction = immutableFunction(n, m_cfg.dpointer ? m_cfg.className : QString());
    m_stream << "!" << m_this << immutablefunction << "(";
    if (!e->param.isEmpty()) {
        m_stream << " i ";
    }
    m_stream << "))";
}

void KConfigCodeGeneratorBase::memberMutatorBody(const CfgEntry *e)
{
    QString n = e->name;
    QString t = e->type;

    // HACK: Don't open '{' manually, use startScope / endScope to automatically handle whitespace indentation.
    if (!e->min.isEmpty()) {
        if (e->min != QLatin1String("0") || !isUnsigned(t)) { // skip writing "if uint<0" (#187579)
            m_stream << whitespace() << "if (v < " << e->min << ")\n";
            m_stream << whitespace() << "{\n";
            m_stream << whitespace(); addDebugMethod(m_stream, m_cfg, n);
            m_stream << ": value \" << v << \" is less than the minimum value of " << e->min << "\";\n";
            m_stream << whitespace() << "  v = " << e->min << ";\n";
            m_stream << whitespace() << "}\n";
        }
    }

    if (!e->max.isEmpty()) {
        m_stream << '\n';
        m_stream << whitespace() << "if (v > " << e->max << ")\n";
        m_stream << whitespace() << "{\n";
        m_stream << whitespace(); addDebugMethod(m_stream, m_cfg, n);
        m_stream << ": value \" << v << \" is greater than the maximum value of " << e->max << "\";\n";
        m_stream << whitespace() << "  v = " << e->max << ";\n";
        m_stream << whitespace() << "}\n\n";
    }

    const QString varExpression = m_this + varPath(n, m_cfg) + (e->param.isEmpty() ? QString() : QStringLiteral("[i]"));

    // TODO: Remove this `hasBody` logic, always use an '{' for the if.
    const bool hasBody = !e->signalList.empty() || m_cfg.generateProperties;

    // m_this call creates an `if (someTest ...) that's just to long to throw over the code.
    createIfSetLogic(e, varExpression);
    m_stream << (hasBody ? " {" : "") << '\n';
    m_stream << whitespace() << "  " << varExpression << " = v;\n";

    const auto listSignal = e->signalList;
    for (const Signal &signal : qAsConst(listSignal)) {
        if (signal.modify) {
            m_stream << whitespace() << "  Q_EMIT " << m_this << signal.name << "();\n";
        } else {
            m_stream << whitespace() <<  "  " << m_this << varPath(QStringLiteral("settingsChanged"), m_cfg)
                << " |= " << signalEnumName(signal.name) << ";\n";
        }
    }
    if (hasBody) {
        m_stream << whitespace() << "}\n";
    }
}
