/****************************************************************************
** Meta object code from reading C++ file 'module_proxy.h'
**
** Created by: The Qt Meta Object Compiler version 69 (Qt 6.9.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../cpp/module_proxy.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'module_proxy.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 69
#error "This file was generated using the moc from 6.9.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN11ModuleProxyE_t {};
} // unnamed namespace

template <> constexpr inline auto ModuleProxy::qt_create_metaobjectdata<qt_meta_tag_ZN11ModuleProxyE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "ModuleProxy",
        "eventResponse",
        "",
        "eventName",
        "QVariantList",
        "data",
        "callRemoteMethod",
        "QVariant",
        "authToken",
        "methodName",
        "args",
        "transportProtocol",
        "informModuleToken",
        "moduleName",
        "token",
        "getPluginMethods",
        "getPluginEvents",
        "getPluginInterface"
    };

    QtMocHelpers::UintData qt_methods {
        // Signal 'eventResponse'
        QtMocHelpers::SignalData<void(const QString &, const QVariantList &)>(1, 2, QMC::AccessPublic, QMetaType::Void, {{
            { QMetaType::QString, 3 }, { 0x80000000 | 4, 5 },
        }}),
        // Method 'callRemoteMethod'
        QtMocHelpers::MethodData<QVariant(const QString &, const QString &, const QVariantList &)>(6, 2, QMC::AccessPublic, 0x80000000 | 7, {{
            { QMetaType::QString, 8 }, { QMetaType::QString, 9 }, { 0x80000000 | 4, 10 },
        }}),
        // Method 'callRemoteMethod'
        QtMocHelpers::MethodData<QVariant(const QString &, const QString &)>(6, 2, QMC::AccessPublic | QMC::MethodCloned, 0x80000000 | 7, {{
            { QMetaType::QString, 8 }, { QMetaType::QString, 9 },
        }}),
        // Method 'callRemoteMethod'
        QtMocHelpers::MethodData<QVariant(const QString &, const QString &, const QVariantList &, const QString &)>(6, 2, QMC::AccessPublic, 0x80000000 | 7, {{
            { QMetaType::QString, 8 }, { QMetaType::QString, 9 }, { 0x80000000 | 4, 10 }, { QMetaType::QString, 11 },
        }}),
        // Method 'informModuleToken'
        QtMocHelpers::MethodData<bool(const QString &, const QString &, const QString &)>(12, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::QString, 8 }, { QMetaType::QString, 13 }, { QMetaType::QString, 14 },
        }}),
        // Method 'getPluginMethods'
        QtMocHelpers::MethodData<QJsonArray()>(15, 2, QMC::AccessPublic, QMetaType::QJsonArray),
        // Method 'getPluginEvents'
        QtMocHelpers::MethodData<QJsonArray()>(16, 2, QMC::AccessPublic, QMetaType::QJsonArray),
        // Method 'getPluginInterface'
        QtMocHelpers::MethodData<QJsonArray()>(17, 2, QMC::AccessPublic, QMetaType::QJsonArray),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<ModuleProxy, qt_meta_tag_ZN11ModuleProxyE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject ModuleProxy::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11ModuleProxyE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11ModuleProxyE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN11ModuleProxyE_t>.metaTypes,
    nullptr
} };

void ModuleProxy::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<ModuleProxy *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->eventResponse((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QVariantList>>(_a[2]))); break;
        case 1: { QVariant _r = _t->callRemoteMethod((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QVariantList>>(_a[3])));
            if (_a[0]) *reinterpret_cast< QVariant*>(_a[0]) = std::move(_r); }  break;
        case 2: { QVariant _r = _t->callRemoteMethod((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2])));
            if (_a[0]) *reinterpret_cast< QVariant*>(_a[0]) = std::move(_r); }  break;
        case 3: { QVariant _r = _t->callRemoteMethod((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QVariantList>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[4])));
            if (_a[0]) *reinterpret_cast< QVariant*>(_a[0]) = std::move(_r); }  break;
        case 4: { bool _r = _t->informModuleToken((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[3])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        case 5: { QJsonArray _r = _t->getPluginMethods();
            if (_a[0]) *reinterpret_cast< QJsonArray*>(_a[0]) = std::move(_r); }  break;
        case 6: { QJsonArray _r = _t->getPluginEvents();
            if (_a[0]) *reinterpret_cast< QJsonArray*>(_a[0]) = std::move(_r); }  break;
        case 7: { QJsonArray _r = _t->getPluginInterface();
            if (_a[0]) *reinterpret_cast< QJsonArray*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        if (QtMocHelpers::indexOfMethod<void (ModuleProxy::*)(const QString & , const QVariantList & )>(_a, &ModuleProxy::eventResponse, 0))
            return;
    }
}

const QMetaObject *ModuleProxy::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ModuleProxy::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN11ModuleProxyE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int ModuleProxy::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 8)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 8;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 8)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 8;
    }
    return _id;
}

// SIGNAL 0
void ModuleProxy::eventResponse(const QString & _t1, const QVariantList & _t2)
{
    QMetaObject::activate<void>(this, &staticMetaObject, 0, nullptr, _t1, _t2);
}
namespace {
struct qt_meta_tag_ZN20ModuleHandshakeProxyE_t {};
} // unnamed namespace

template <> constexpr inline auto ModuleHandshakeProxy::qt_create_metaobjectdata<qt_meta_tag_ZN20ModuleHandshakeProxyE_t>()
{
    namespace QMC = QtMocConstants;
    QtMocHelpers::StringRefStorage qt_stringData {
        "ModuleHandshakeProxy",
        "informModuleToken",
        "",
        "authToken",
        "moduleName",
        "token"
    };

    QtMocHelpers::UintData qt_methods {
        // Method 'informModuleToken'
        QtMocHelpers::MethodData<bool(const QString &, const QString &, const QString &)>(1, 2, QMC::AccessPublic, QMetaType::Bool, {{
            { QMetaType::QString, 3 }, { QMetaType::QString, 4 }, { QMetaType::QString, 5 },
        }}),
    };
    QtMocHelpers::UintData qt_properties {
    };
    QtMocHelpers::UintData qt_enums {
    };
    return QtMocHelpers::metaObjectData<ModuleHandshakeProxy, qt_meta_tag_ZN20ModuleHandshakeProxyE_t>(QMC::MetaObjectFlag{}, qt_stringData,
            qt_methods, qt_properties, qt_enums);
}
Q_CONSTINIT const QMetaObject ModuleHandshakeProxy::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN20ModuleHandshakeProxyE_t>.stringdata,
    qt_staticMetaObjectStaticContent<qt_meta_tag_ZN20ModuleHandshakeProxyE_t>.data,
    qt_static_metacall,
    nullptr,
    qt_staticMetaObjectRelocatingContent<qt_meta_tag_ZN20ModuleHandshakeProxyE_t>.metaTypes,
    nullptr
} };

void ModuleHandshakeProxy::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<ModuleHandshakeProxy *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: { bool _r = _t->informModuleToken((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[3])));
            if (_a[0]) *reinterpret_cast< bool*>(_a[0]) = std::move(_r); }  break;
        default: ;
        }
    }
}

const QMetaObject *ModuleHandshakeProxy::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ModuleHandshakeProxy::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_staticMetaObjectStaticContent<qt_meta_tag_ZN20ModuleHandshakeProxyE_t>.strings))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int ModuleHandshakeProxy::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 1)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 1;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 1)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 1;
    }
    return _id;
}
QT_WARNING_POP
