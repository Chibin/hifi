//
//  ScriptEngine.cpp
//  libraries/script-engine/src
//
//  Created by Brad Hefta-Gaub on 12/14/13.
//  Copyright 2013 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include <chrono>
#include <thread>

#include <QtCore/QCoreApplication>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtCore/QThread>
#include <QtNetwork/QNetworkRequest>
#include <QtNetwork/QNetworkReply>
#include <QtScript/QScriptEngine>
#include <QtScript/QScriptValue>
#include <QtScript/QScriptValueIterator>
#include <QtCore/QStringList>

#include <AudioConstants.h>
#include <AudioEffectOptions.h>
#include <AvatarData.h>
#include <EntityScriptingInterface.h>
#include <MessagesClient.h>
#include <NetworkAccessManager.h>
#include <ResourceScriptingInterface.h>
#include <NodeList.h>
#include <udt/PacketHeaders.h>
#include <UUID.h>

#include <controllers/ScriptingInterface.h>
#include <AnimationObject.h>

#include "ArrayBufferViewClass.h"
#include "BatchLoader.h"
#include "DataViewClass.h"
#include "EventTypes.h"
#include "MenuItemProperties.h"
#include "ScriptAudioInjector.h"
#include "ScriptCache.h"
#include "ScriptEngineLogging.h"
#include "ScriptEngine.h"
#include "TypedArrays.h"
#include "XMLHttpRequestClass.h"
#include "WebSocketClass.h"
#include "RecordingScriptingInterface.h"
#include "ScriptEngines.h"

#include "MIDIEvent.h"

std::atomic<bool> ScriptEngine::_stoppingAllScripts { false };

static const QString SCRIPT_EXCEPTION_FORMAT = "[UncaughtException] %1 in %2:%3";

Q_DECLARE_METATYPE(QScriptEngine::FunctionSignature)
static int functionSignatureMetaID = qRegisterMetaType<QScriptEngine::FunctionSignature>();

static QScriptValue debugPrint(QScriptContext* context, QScriptEngine* engine){
    QString message = "";
    for (int i = 0; i < context->argumentCount(); i++) {
        if (i > 0) {
            message += " ";
        }
        message += context->argument(i).toString();
    }
    qCDebug(scriptengine).noquote() << "script:print()<<" << message;  // noquote() so that \n is treated as newline

    message = message.replace("\\", "\\\\")
                     .replace("\n", "\\n")
                     .replace("\r", "\\r")
                     .replace("'", "\\'");
    engine->evaluate("Script.print('" + message + "')");

    return QScriptValue();
}

QScriptValue avatarDataToScriptValue(QScriptEngine* engine, AvatarData* const &in) {
    return engine->newQObject(in);
}

void avatarDataFromScriptValue(const QScriptValue &object, AvatarData* &out) {
    out = qobject_cast<AvatarData*>(object.toQObject());
}

Q_DECLARE_METATYPE(controller::InputController*)
//static int inputControllerPointerId = qRegisterMetaType<controller::InputController*>();

QScriptValue inputControllerToScriptValue(QScriptEngine *engine, controller::InputController* const &in) {
    return engine->newQObject(in);
}

void inputControllerFromScriptValue(const QScriptValue &object, controller::InputController* &out) {
    out = qobject_cast<controller::InputController*>(object.toQObject());
}

static bool hasCorrectSyntax(const QScriptProgram& program) {
    const auto syntaxCheck = QScriptEngine::checkSyntax(program.sourceCode());
    if (syntaxCheck.state() != QScriptSyntaxCheckResult::Valid) {
        const auto error = syntaxCheck.errorMessage();
        const auto line = QString::number(syntaxCheck.errorLineNumber());
        const auto column = QString::number(syntaxCheck.errorColumnNumber());
        const auto message = QString("[SyntaxError] %1 in %2:%3(%4)").arg(error, program.fileName(), line, column);
        qCWarning(scriptengine) << qPrintable(message);
        return false;
    }
    return true;
}

static bool hadUncaughtExceptions(QScriptEngine& engine, const QString& fileName) {
    if (engine.hasUncaughtException()) {
        const auto backtrace = engine.uncaughtExceptionBacktrace();
        const auto exception = engine.uncaughtException().toString();
        const auto line = QString::number(engine.uncaughtExceptionLineNumber());
        engine.clearExceptions();

        auto message = QString(SCRIPT_EXCEPTION_FORMAT).arg(exception, fileName, line);
        if (!backtrace.empty()) {
            static const auto lineSeparator = "\n    ";
            message += QString("\n[Backtrace]%1%2").arg(lineSeparator, backtrace.join(lineSeparator));
        }
        qCWarning(scriptengine) << qPrintable(message);
        return true;
    }
    return false;
}

ScriptEngine::ScriptEngine(const QString& scriptContents, const QString& fileNameString, bool wantSignals) :
    _scriptContents(scriptContents),
    _timerFunctionMap(),
    _wantSignals(wantSignals),
    _fileNameString(fileNameString),
    _arrayBufferClass(new ArrayBufferClass(this))
{
    DependencyManager::get<ScriptEngines>()->addScriptEngine(this);

    connect(this, &QScriptEngine::signalHandlerException, this, [this](const QScriptValue& exception) {
        hadUncaughtExceptions(*this, _fileNameString);
    });
    
    setProcessEventsInterval(MSECS_PER_SECOND);
}

ScriptEngine::~ScriptEngine() {
    qCDebug(scriptengine) << "Script Engine shutting down (destructor) for script:" << getFilename();

    auto scriptEngines = DependencyManager::get<ScriptEngines>();
    if (scriptEngines) {
        scriptEngines->removeScriptEngine(this);
    } else {
        qCWarning(scriptengine) << "Script destroyed after ScriptEngines!";
    }

    waitTillDoneRunning();
}

void ScriptEngine::disconnectNonEssentialSignals() {
    disconnect();
    QThread* receiver;
    // Ensure the thread should be running, and does exist
    if (_isRunning && _isThreaded && (receiver = thread())) {
        connect(this, &ScriptEngine::doneRunning, receiver, &QThread::quit);
    }
}

void ScriptEngine::runInThread() {
    Q_ASSERT_X(!_isThreaded, "ScriptEngine::runInThread()", "runInThread should not be called more than once");

    if (_isThreaded) {
        qCWarning(scriptengine) << "ScriptEngine already running in thread: " << getFilename();
        return;
    }

    _isThreaded = true;
    QThread* workerThread = new QThread();
    QString scriptEngineName = QString("Script Thread:") + getFilename();
    workerThread->setObjectName(scriptEngineName);

    // NOTE: If you connect any essential signals for proper shutdown or cleanup of
    // the script engine, make sure to add code to "reconnect" them to the
    // disconnectNonEssentialSignals() method

    // when the worker thread is started, call our engine's run..
    connect(workerThread, &QThread::started, this, &ScriptEngine::run);

    // tell the thread to stop when the script engine is done
    connect(this, &ScriptEngine::doneRunning, workerThread, &QThread::quit);

    moveToThread(workerThread);

    // Starts an event loop, and emits workerThread->started()
    workerThread->start();
}

void ScriptEngine::waitTillDoneRunning() {
    // If the script never started running or finished running before we got here, we don't need to wait for it
    auto workerThread = thread();
    if (_isThreaded && workerThread) {
        QString scriptName = getFilename();
        auto startedWaiting = usecTimestampNow();

        while (workerThread->isRunning()) {
            // NOTE: This will be called on the main application thread from stopAllScripts.
            //       The application thread will need to continue to process events, because
            //       the scripts will likely need to marshall messages across to the main thread, e.g.
            //       if they access Settings or Menu in any of their shutdown code. So:
            // Process events for the main application thread, allowing invokeMethod calls to pass between threads.
            QCoreApplication::processEvents(); // thread-safe :)

            // If we've been waiting a second or more, then tell the script engine to stop evaluating
            static const auto MAX_SCRIPT_EVALUATION_TIME =  USECS_PER_SECOND;
            auto elapsedUsecs = usecTimestampNow() - startedWaiting;
            if (elapsedUsecs > MAX_SCRIPT_EVALUATION_TIME) {
                qCDebug(scriptengine) <<
                    "Script " << scriptName << " has been running too long [" << elapsedUsecs << " usecs] quitting.";
                abortEvaluation(); // to allow the thread to quit
                workerThread->quit();
                break;
            }

            // Avoid a pure busy wait
            QThread::yieldCurrentThread();
        }

        workerThread->deleteLater();
    }
}

QString ScriptEngine::getFilename() const {
    QStringList fileNameParts = _fileNameString.split("/");
    QString lastPart;
    if (!fileNameParts.isEmpty()) {
        lastPart = fileNameParts.last();
    }
    return lastPart;
}


// FIXME - switch this to the new model of ScriptCache callbacks
void ScriptEngine::loadURL(const QUrl& scriptURL, bool reload) {
    if (_isRunning) {
        return;
    }

    QUrl url = expandScriptUrl(scriptURL);
    _fileNameString = url.toString();
    _isReloading = reload;

    bool isPending;
    auto scriptCache = DependencyManager::get<ScriptCache>();
    scriptCache->getScript(url, this, isPending, reload);
}

// FIXME - switch this to the new model of ScriptCache callbacks
void ScriptEngine::scriptContentsAvailable(const QUrl& url, const QString& scriptContents) {
    _scriptContents = scriptContents;
    if (_wantSignals) {
        emit scriptLoaded(url.toString());
    }
}

// FIXME - switch this to the new model of ScriptCache callbacks
void ScriptEngine::errorInLoadingScript(const QUrl& url) {
    qCDebug(scriptengine) << "ERROR Loading file:" << url.toString() << "line:" << __LINE__;
    if (_wantSignals) {
        emit errorLoadingScript(_fileNameString); // ??
    }
}

// Even though we never pass AnimVariantMap directly to and from javascript, the queued invokeMethod of
// callAnimationStateHandler requires that the type be registered.
// These two are meaningful, if we ever do want to use them...
static QScriptValue animVarMapToScriptValue(QScriptEngine* engine, const AnimVariantMap& parameters) {
    QStringList unused;
    return parameters.animVariantMapToScriptValue(engine, unused, false);
}
static void animVarMapFromScriptValue(const QScriptValue& value, AnimVariantMap& parameters) {
    parameters.animVariantMapFromScriptValue(value);
}
// ... while these two are not. But none of the four are ever used.
static QScriptValue resultHandlerToScriptValue(QScriptEngine* engine, const AnimVariantResultHandler& resultHandler) {
    qCCritical(scriptengine) << "Attempt to marshall result handler to javascript";
    assert(false);
    return QScriptValue();
}
static void resultHandlerFromScriptValue(const QScriptValue& value, AnimVariantResultHandler& resultHandler) {
    qCCritical(scriptengine) << "Attempt to marshall result handler from javascript";
    assert(false);
}

// Templated qScriptRegisterMetaType fails to compile with raw pointers
using ScriptableResourceRawPtr = ScriptableResource*;

static QScriptValue scriptableResourceToScriptValue(QScriptEngine* engine, const ScriptableResourceRawPtr& resource) {
    // The first script to encounter this resource will track its memory.
    // In this way, it will be more likely to GC.
    // This fails in the case that the resource is used across many scripts, but
    // in that case it would be too difficult to tell which one should track the memory, and
    // this serves the common case (use in a single script).
    auto data = resource->getResource();
    if (data && !resource->isInScript()) {
        resource->setInScript(true);
        QObject::connect(data.data(), SIGNAL(updateSize(qint64)), engine, SLOT(updateMemoryCost(qint64)));
    }

    auto object = engine->newQObject(
        const_cast<ScriptableResourceRawPtr>(resource),
        QScriptEngine::ScriptOwnership);
    return object;
}

static void scriptableResourceFromScriptValue(const QScriptValue& value, ScriptableResourceRawPtr& resource) {
    resource = static_cast<ScriptableResourceRawPtr>(value.toQObject());
}

static QScriptValue createScriptableResourcePrototype(QScriptEngine* engine) {
    auto prototype = engine->newObject();

    // Expose enum State to JS/QML via properties
    QObject* state = new QObject(engine);
    state->setObjectName("ResourceState");
    auto metaEnum = QMetaEnum::fromType<ScriptableResource::State>();
    for (int i = 0; i < metaEnum.keyCount(); ++i) {
        state->setProperty(metaEnum.key(i), metaEnum.value(i));
    }

    auto prototypeState = engine->newQObject(state, QScriptEngine::QtOwnership, QScriptEngine::ExcludeSlots | QScriptEngine::ExcludeSuperClassMethods);
    prototype.setProperty("State", prototypeState);

    return prototype;
}

void ScriptEngine::init() {
    if (_isInitialized) {
        return; // only initialize once
    }

    _isInitialized = true;

    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();
    entityScriptingInterface->init();

    // register various meta-types
    registerMetaTypes(this);
    registerMIDIMetaTypes(this);
    registerEventTypes(this);
    registerMenuItemProperties(this);
    registerAnimationTypes(this);
    registerAvatarTypes(this);
    registerAudioMetaTypes(this);

    qScriptRegisterMetaType(this, EntityPropertyFlagsToScriptValue, EntityPropertyFlagsFromScriptValue);
    qScriptRegisterMetaType(this, EntityItemPropertiesToScriptValue, EntityItemPropertiesFromScriptValueHonorReadOnly);
    qScriptRegisterMetaType(this, EntityItemIDtoScriptValue, EntityItemIDfromScriptValue);
    qScriptRegisterMetaType(this, RayToEntityIntersectionResultToScriptValue, RayToEntityIntersectionResultFromScriptValue);
    qScriptRegisterSequenceMetaType<QVector<QUuid>>(this);
    qScriptRegisterSequenceMetaType<QVector<EntityItemID>>(this);

    qScriptRegisterSequenceMetaType<QVector<glm::vec2> >(this);
    qScriptRegisterSequenceMetaType<QVector<glm::quat> >(this);
    qScriptRegisterSequenceMetaType<QVector<QString> >(this);

    QScriptValue xmlHttpRequestConstructorValue = newFunction(XMLHttpRequestClass::constructor);
    globalObject().setProperty("XMLHttpRequest", xmlHttpRequestConstructorValue);

    QScriptValue webSocketConstructorValue = newFunction(WebSocketClass::constructor);
    globalObject().setProperty("WebSocket", webSocketConstructorValue);

    QScriptValue printConstructorValue = newFunction(debugPrint);
    globalObject().setProperty("print", printConstructorValue);

    QScriptValue audioEffectOptionsConstructorValue = newFunction(AudioEffectOptions::constructor);
    globalObject().setProperty("AudioEffectOptions", audioEffectOptionsConstructorValue);

    qScriptRegisterMetaType(this, injectorToScriptValue, injectorFromScriptValue);
    qScriptRegisterMetaType(this, inputControllerToScriptValue, inputControllerFromScriptValue);
    qScriptRegisterMetaType(this, avatarDataToScriptValue, avatarDataFromScriptValue);
    qScriptRegisterMetaType(this, animationDetailsToScriptValue, animationDetailsFromScriptValue);
    qScriptRegisterMetaType(this, webSocketToScriptValue, webSocketFromScriptValue);
    qScriptRegisterMetaType(this, qWSCloseCodeToScriptValue, qWSCloseCodeFromScriptValue);
    qScriptRegisterMetaType(this, wscReadyStateToScriptValue, wscReadyStateFromScriptValue);

    registerGlobalObject("Script", this);
    registerGlobalObject("Audio", &AudioScriptingInterface::getInstance());
    registerGlobalObject("Entities", entityScriptingInterface.data());
    registerGlobalObject("Quat", &_quatLibrary);
    registerGlobalObject("Vec3", &_vec3Library);
    registerGlobalObject("Mat4", &_mat4Library);
    registerGlobalObject("Uuid", &_uuidLibrary);
    registerGlobalObject("Messages", DependencyManager::get<MessagesClient>().data());
    qScriptRegisterMetaType(this, animVarMapToScriptValue, animVarMapFromScriptValue);
    qScriptRegisterMetaType(this, resultHandlerToScriptValue, resultHandlerFromScriptValue);

    // Scriptable cache access
    auto resourcePrototype = createScriptableResourcePrototype(this);
    globalObject().setProperty("Resource", resourcePrototype);
    setDefaultPrototype(qMetaTypeId<ScriptableResource*>(), resourcePrototype);
    qScriptRegisterMetaType(this, scriptableResourceToScriptValue, scriptableResourceFromScriptValue);

    // constants
    globalObject().setProperty("TREE_SCALE", newVariant(QVariant(TREE_SCALE)));

    auto scriptingInterface = DependencyManager::get<controller::ScriptingInterface>();
    registerGlobalObject("Controller", scriptingInterface.data());
    UserInputMapper::registerControllerTypes(this);

    auto recordingInterface = DependencyManager::get<RecordingScriptingInterface>();
    registerGlobalObject("Recording", recordingInterface.data());

    registerGlobalObject("Assets", &_assetScriptingInterface);
    registerGlobalObject("Resources", DependencyManager::get<ResourceScriptingInterface>().data());
}

void ScriptEngine::registerValue(const QString& valueName, QScriptValue value) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::registerValue() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]";
#endif
        QMetaObject::invokeMethod(this, "registerValue",
                                  Q_ARG(const QString&, valueName),
                                  Q_ARG(QScriptValue, value));
        return;
    }

    QStringList pathToValue = valueName.split(".");
    int partsToGo = pathToValue.length();
    QScriptValue partObject = globalObject();

    for (const auto& pathPart : pathToValue) {
        partsToGo--;
        if (!partObject.property(pathPart).isValid()) {
            if (partsToGo > 0) {
                //QObject *object = new QObject;
                QScriptValue partValue = newArray(); //newQObject(object, QScriptEngine::ScriptOwnership);
                partObject.setProperty(pathPart, partValue);
            } else {
                partObject.setProperty(pathPart, value);
            }
        }
        partObject = partObject.property(pathPart);
    }
}

void ScriptEngine::registerGlobalObject(const QString& name, QObject* object) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::registerGlobalObject() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]  name:" << name;
#endif
        QMetaObject::invokeMethod(this, "registerGlobalObject",
                                  Q_ARG(const QString&, name),
                                  Q_ARG(QObject*, object));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::registerGlobalObject() called on thread [" << QThread::currentThread() << "] name:" << name;
#endif

    if (!globalObject().property(name).isValid()) {
        if (object) {
            QScriptValue value = newQObject(object);
            globalObject().setProperty(name, value);
        } else {
            globalObject().setProperty(name, QScriptValue());
        }
    }
}

void ScriptEngine::registerFunction(const QString& name, QScriptEngine::FunctionSignature functionSignature, int numArguments) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::registerFunction() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "] name:" << name;
#endif
        QMetaObject::invokeMethod(this, "registerFunction",
                                  Q_ARG(const QString&, name),
                                  Q_ARG(QScriptEngine::FunctionSignature, functionSignature),
                                  Q_ARG(int, numArguments));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::registerFunction() called on thread [" << QThread::currentThread() << "] name:" << name;
#endif

    QScriptValue scriptFun = newFunction(functionSignature, numArguments);
    globalObject().setProperty(name, scriptFun);
}

void ScriptEngine::registerFunction(const QString& parent, const QString& name, QScriptEngine::FunctionSignature functionSignature, int numArguments) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::registerFunction() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "] parent:" << parent << "name:" << name;
#endif
        QMetaObject::invokeMethod(this, "registerFunction",
                                  Q_ARG(const QString&, name),
                                  Q_ARG(QScriptEngine::FunctionSignature, functionSignature),
                                  Q_ARG(int, numArguments));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::registerFunction() called on thread [" << QThread::currentThread() << "] parent:" << parent << "name:" << name;
#endif

    QScriptValue object = globalObject().property(parent);
    if (object.isValid()) {
        QScriptValue scriptFun = newFunction(functionSignature, numArguments);
        object.setProperty(name, scriptFun);
    }
}

void ScriptEngine::registerGetterSetter(const QString& name, QScriptEngine::FunctionSignature getter,
                                        QScriptEngine::FunctionSignature setter, const QString& parent) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::registerGetterSetter() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "] "
            " name:" << name << "parent:" << parent;
#endif
        QMetaObject::invokeMethod(this, "registerGetterSetter",
                                  Q_ARG(const QString&, name),
                                  Q_ARG(QScriptEngine::FunctionSignature, getter),
                                  Q_ARG(QScriptEngine::FunctionSignature, setter),
                                  Q_ARG(const QString&, parent));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::registerGetterSetter() called on thread [" << QThread::currentThread() << "] name:" << name << "parent:" << parent;
#endif

    QScriptValue setterFunction = newFunction(setter, 1);
    QScriptValue getterFunction = newFunction(getter);

    if (!parent.isNull() && !parent.isEmpty()) {
        QScriptValue object = globalObject().property(parent);
        if (object.isValid()) {
            object.setProperty(name, setterFunction, QScriptValue::PropertySetter);
            object.setProperty(name, getterFunction, QScriptValue::PropertyGetter);
        }
    } else {
        globalObject().setProperty(name, setterFunction, QScriptValue::PropertySetter);
        globalObject().setProperty(name, getterFunction, QScriptValue::PropertyGetter);
    }
}

// Unregister the handlers for this eventName and entityID.
void ScriptEngine::removeEventHandler(const EntityItemID& entityID, const QString& eventName, QScriptValue handler) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::removeEventHandler() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "] "
            "entityID:" << entityID << " eventName:" << eventName;
#endif
        QMetaObject::invokeMethod(this, "removeEventHandler",
                                  Q_ARG(const EntityItemID&, entityID),
                                  Q_ARG(const QString&, eventName),
                                  Q_ARG(QScriptValue, handler));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::removeEventHandler() called on thread [" << QThread::currentThread() << "] entityID:" << entityID << " eventName : " << eventName;
#endif

    if (!_registeredHandlers.contains(entityID)) {
        return;
    }
    RegisteredEventHandlers& handlersOnEntity = _registeredHandlers[entityID];
    CallbackList& handlersForEvent = handlersOnEntity[eventName];
    // QScriptValue does not have operator==(), so we can't use QList::removeOne and friends. So iterate.
    for (int i = 0; i < handlersForEvent.count(); ++i) {
        if (handlersForEvent[i].function.equals(handler)) {
            handlersForEvent.removeAt(i);
            return; // Design choice: since comparison is relatively expensive, just remove the first matching handler.
        }
    }
}
// Register the handler.
void ScriptEngine::addEventHandler(const EntityItemID& entityID, const QString& eventName, QScriptValue handler) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::addEventHandler() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "] "
        "entityID:" << entityID << " eventName:" << eventName;
#endif

        QMetaObject::invokeMethod(this, "addEventHandler",
                                  Q_ARG(const EntityItemID&, entityID),
                                  Q_ARG(const QString&, eventName),
                                  Q_ARG(QScriptValue, handler));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::addEventHandler() called on thread [" << QThread::currentThread() << "] entityID:" << entityID << " eventName : " << eventName;
#endif

    if (_registeredHandlers.count() == 0) { // First time any per-entity handler has been added in this script...
        // Connect up ALL the handlers to the global entities object's signals.
        // (We could go signal by signal, or even handler by handler, but I don't think the efficiency is worth the complexity.)
        auto entities = DependencyManager::get<EntityScriptingInterface>();
        // Bug? These handlers are deleted when entityID is deleted, which is nice.
        // But if they are created by an entity script on a different entity, should they also be deleted when the entity script unloads?
        // E.g., suppose a bow has an entity script that causes arrows to be created with a potential lifetime greater than the bow,
        // and that the entity script adds (e.g., collision) handlers to the arrows. Should those handlers fire if the bow is unloaded?
        // Also, what about when the entity script is REloaded?
        // For now, we are leaving them around. Changing that would require some non-trivial digging around to find the
        // handlers that were added while a given currentEntityIdentifier was in place. I don't think this is dangerous. Just perhaps unexpected. -HRS
        connect(entities.data(), &EntityScriptingInterface::deletingEntity, this, [this](const EntityItemID& entityID) {
            _registeredHandlers.remove(entityID);
        });

        // Two common cases of event handler, differing only in argument signature.
        using SingleEntityHandler = std::function<void(const EntityItemID&)>;
        auto makeSingleEntityHandler = [this](QString eventName) -> SingleEntityHandler {
            return [this, eventName](const EntityItemID& entityItemID) {
                forwardHandlerCall(entityItemID, eventName, { entityItemID.toScriptValue(this) });
            };
        };

        using MouseHandler = std::function<void(const EntityItemID&, const MouseEvent&)>;
        auto makeMouseHandler = [this](QString eventName) -> MouseHandler {
            return [this, eventName](const EntityItemID& entityItemID, const MouseEvent& event) {
                forwardHandlerCall(entityItemID, eventName, { entityItemID.toScriptValue(this), event.toScriptValue(this) });
            };
        };

        using CollisionHandler = std::function<void(const EntityItemID&, const EntityItemID&, const Collision&)>;
        auto makeCollisionHandler = [this](QString eventName) -> CollisionHandler {
            return [this, eventName](const EntityItemID& idA, const EntityItemID& idB, const Collision& collision) {
                forwardHandlerCall(idA, eventName, { idA.toScriptValue(this), idB.toScriptValue(this),
                    collisionToScriptValue(this, collision) });
            };
        };

        connect(entities.data(), &EntityScriptingInterface::enterEntity, this, makeSingleEntityHandler("enterEntity"));
        connect(entities.data(), &EntityScriptingInterface::leaveEntity, this, makeSingleEntityHandler("leaveEntity"));

        connect(entities.data(), &EntityScriptingInterface::mousePressOnEntity, this, makeMouseHandler("mousePressOnEntity"));
        connect(entities.data(), &EntityScriptingInterface::mouseMoveOnEntity, this, makeMouseHandler("mouseMoveOnEntity"));
        connect(entities.data(), &EntityScriptingInterface::mouseReleaseOnEntity, this, makeMouseHandler("mouseReleaseOnEntity"));

        connect(entities.data(), &EntityScriptingInterface::clickDownOnEntity, this, makeMouseHandler("clickDownOnEntity"));
        connect(entities.data(), &EntityScriptingInterface::holdingClickOnEntity, this, makeMouseHandler("holdingClickOnEntity"));
        connect(entities.data(), &EntityScriptingInterface::clickReleaseOnEntity, this, makeMouseHandler("clickReleaseOnEntity"));

        connect(entities.data(), &EntityScriptingInterface::hoverEnterEntity, this, makeMouseHandler("hoverEnterEntity"));
        connect(entities.data(), &EntityScriptingInterface::hoverOverEntity, this, makeMouseHandler("hoverOverEntity"));
        connect(entities.data(), &EntityScriptingInterface::hoverLeaveEntity, this, makeMouseHandler("hoverLeaveEntity"));

        connect(entities.data(), &EntityScriptingInterface::collisionWithEntity, this, makeCollisionHandler("collisionWithEntity"));
    }
    if (!_registeredHandlers.contains(entityID)) {
        _registeredHandlers[entityID] = RegisteredEventHandlers();
    }
    CallbackList& handlersForEvent = _registeredHandlers[entityID][eventName];
    CallbackData handlerData = {handler, currentEntityIdentifier, currentSandboxURL};
    handlersForEvent << handlerData; // Note that the same handler can be added many times. See removeEntityEventHandler().
}


QScriptValue ScriptEngine::evaluate(const QString& sourceCode, const QString& fileName, int lineNumber) {
    if (_stoppingAllScripts) {
        return QScriptValue(); // bail early
    }

    if (QThread::currentThread() != thread()) {
        QScriptValue result;
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::evaluate() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "] "
            "sourceCode:" << sourceCode << " fileName:" << fileName << "lineNumber:" << lineNumber;
#endif
        QMetaObject::invokeMethod(this, "evaluate", Qt::BlockingQueuedConnection,
                                  Q_RETURN_ARG(QScriptValue, result),
                                  Q_ARG(const QString&, sourceCode),
                                  Q_ARG(const QString&, fileName),
                                  Q_ARG(int, lineNumber));
        return result;
    }

    // Check syntax
    const QScriptProgram program(sourceCode, fileName, lineNumber);
    if (!hasCorrectSyntax(program)) {
        return QScriptValue();
    }

    ++_evaluatesPending;
    const auto result = QScriptEngine::evaluate(program);
    --_evaluatesPending;

    const auto hadUncaughtException = hadUncaughtExceptions(*this, program.fileName());
    if (_wantSignals) {
        emit evaluationFinished(result, hadUncaughtException);
    }
    return result;
}

void ScriptEngine::run() {
    if (_stoppingAllScripts) {
        return; // bail early - avoid setting state in init(), as evaluate() will bail too
    }

    if (!_isInitialized) {
        init();
    }

    _isRunning = true;
    if (_wantSignals) {
        emit runningStateChanged();
    }

    QScriptValue result = evaluate(_scriptContents, _fileNameString);

#ifdef _WIN32
    // VS13 does not sleep_until unless it uses the system_clock, see:
    // https://www.reddit.com/r/cpp_questions/comments/3o71ic/sleep_until_not_working_with_a_time_pointsteady/
    using clock = std::chrono::system_clock;
#else
    using clock = std::chrono::high_resolution_clock;
#endif

    clock::time_point startTime = clock::now();
    int thisFrame = 0;

    auto nodeList = DependencyManager::get<NodeList>();
    auto entityScriptingInterface = DependencyManager::get<EntityScriptingInterface>();

    qint64 lastUpdate = usecTimestampNow();

    // TODO: Integrate this with signals/slots instead of reimplementing throttling for ScriptEngine
    while (!_isFinished) {
        // Throttle to SCRIPT_FPS
        const std::chrono::microseconds FRAME_DURATION(USECS_PER_SECOND / SCRIPT_FPS + 1);
        clock::time_point sleepTime(startTime + thisFrame++ * FRAME_DURATION);
        std::this_thread::sleep_until(sleepTime);

#ifdef SCRIPT_DELAY_DEBUG
        {
            auto now = clock::now();
            uint64_t seconds = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();
            if (seconds > 0) { // avoid division by zero and time travel
                uint64_t fps = thisFrame / seconds;
                // Overreporting artificially reduces the reported rate
                if (thisFrame % SCRIPT_FPS == 0) {
                    qCDebug(scriptengine) <<
                        "Frame:" << thisFrame <<
                        "Slept (us):" << std::chrono::duration_cast<std::chrono::microseconds>(now - sleepTime).count() <<
                        "FPS:" << fps;
                }
            }
        }
#endif
        if (_isFinished) {
            break;
        }

        QCoreApplication::processEvents();

        if (_isFinished) {
            break;
        }

        if (!_isFinished && entityScriptingInterface->getEntityPacketSender()->serversExist()) {
            // release the queue of edit entity messages.
            entityScriptingInterface->getEntityPacketSender()->releaseQueuedMessages();

            // since we're in non-threaded mode, call process so that the packets are sent
            if (!entityScriptingInterface->getEntityPacketSender()->isThreaded()) {
                entityScriptingInterface->getEntityPacketSender()->process();
            }
        }

        qint64 now = usecTimestampNow();

        // we check for 'now' in the past in case people set their clock back
        if (lastUpdate < now) {
            float deltaTime = (float) (now - lastUpdate) / (float) USECS_PER_SECOND;
            if (!_isFinished) {
                if (_wantSignals) {
                    emit update(deltaTime);
                }
            }
        }
        lastUpdate = now;

        // Debug and clear exceptions
        hadUncaughtExceptions(*this, _fileNameString);
    }

    stopAllTimers(); // make sure all our timers are stopped if the script is ending
    if (_wantSignals) {
        emit scriptEnding();
    }

    if (entityScriptingInterface->getEntityPacketSender()->serversExist()) {
        // release the queue of edit entity messages.
        entityScriptingInterface->getEntityPacketSender()->releaseQueuedMessages();

        // since we're in non-threaded mode, call process so that the packets are sent
        if (!entityScriptingInterface->getEntityPacketSender()->isThreaded()) {
            // wait here till the edit packet sender is completely done sending
            while (entityScriptingInterface->getEntityPacketSender()->hasPacketsToSend()) {
                entityScriptingInterface->getEntityPacketSender()->process();
                QCoreApplication::processEvents();
            }
        } else {
            // FIXME - do we need to have a similar "wait here" loop for non-threaded packet senders?
        }
    }

    if (_wantSignals) {
        emit finished(_fileNameString, this);
    }

    _isRunning = false;
    if (_wantSignals) {
        emit runningStateChanged();
        emit doneRunning();
    }
}

// NOTE: This is private because it must be called on the same thread that created the timers, which is why
// we want to only call it in our own run "shutdown" processing.
void ScriptEngine::stopAllTimers() {
    QMutableHashIterator<QTimer*, CallbackData> i(_timerFunctionMap);
    while (i.hasNext()) {
        i.next();
        QTimer* timer = i.key();
        stopTimer(timer);
    }
}
void ScriptEngine::stopAllTimersForEntityScript(const EntityItemID& entityID) {
     // We could maintain a separate map of entityID => QTimer, but someone will have to prove to me that it's worth the complexity. -HRS
    QVector<QTimer*> toDelete;
    QMutableHashIterator<QTimer*, CallbackData> i(_timerFunctionMap);
    while (i.hasNext()) {
        i.next();
        if (i.value().definingEntityIdentifier != entityID) {
            continue;
        }
        QTimer* timer = i.key();
        toDelete << timer; // don't delete while we're iterating. save it.
    }
    for (auto timer:toDelete) { // now reap 'em
        stopTimer(timer);
    }

}

void ScriptEngine::stop() {
    if (!_isFinished) {
        if (QThread::currentThread() != thread()) {
            QMetaObject::invokeMethod(this, "stop");
            return;
        }
        _isFinished = true;
        if (_wantSignals) {
            emit runningStateChanged();
        }
    }
}

// Other threads can invoke this through invokeMethod, which causes the callback to be asynchronously executed in this script's thread.
void ScriptEngine::callAnimationStateHandler(QScriptValue callback, AnimVariantMap parameters, QStringList names, bool useNames, AnimVariantResultHandler resultHandler) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::callAnimationStateHandler() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]  name:" << name;
#endif
        QMetaObject::invokeMethod(this, "callAnimationStateHandler",
                                  Q_ARG(QScriptValue, callback),
                                  Q_ARG(AnimVariantMap, parameters),
                                  Q_ARG(QStringList, names),
                                  Q_ARG(bool, useNames),
                                  Q_ARG(AnimVariantResultHandler, resultHandler));
        return;
    }
    QScriptValue javascriptParameters = parameters.animVariantMapToScriptValue(this, names, useNames);
    QScriptValueList callingArguments;
    callingArguments << javascriptParameters;
    assert(currentEntityIdentifier.isInvalidID()); // No animation state handlers from entity scripts.
    QScriptValue result = callback.call(QScriptValue(), callingArguments);

    // validate result from callback function.
    if (result.isValid() && result.isObject()) {
        resultHandler(result);
    } else {
        qCWarning(scriptengine) << "ScriptEngine::callAnimationStateHandler invalid return argument from callback, expected an object";
    }
}

void ScriptEngine::updateMemoryCost(const qint64& deltaSize) {
    if (deltaSize > 0) {
        reportAdditionalMemoryCost(deltaSize);
    }
}

void ScriptEngine::timerFired() {
    QTimer* callingTimer = reinterpret_cast<QTimer*>(sender());
    CallbackData timerData = _timerFunctionMap.value(callingTimer);

    if (!callingTimer->isActive()) {
        // this timer is done, we can kill it
        _timerFunctionMap.remove(callingTimer);
        delete callingTimer;
    }

    // call the associated JS function, if it exists
    if (timerData.function.isValid()) {
        callWithEnvironment(timerData.definingEntityIdentifier, timerData.definingSandboxURL, timerData.function, timerData.function, QScriptValueList());
    }
}


QObject* ScriptEngine::setupTimerWithInterval(const QScriptValue& function, int intervalMS, bool isSingleShot) {
    // create the timer, add it to the map, and start it
    QTimer* newTimer = new QTimer(this);
    newTimer->setSingleShot(isSingleShot);

    connect(newTimer, &QTimer::timeout, this, &ScriptEngine::timerFired);

    // make sure the timer stops when the script does
    connect(this, &ScriptEngine::scriptEnding, newTimer, &QTimer::stop);

    CallbackData timerData = {function, currentEntityIdentifier, currentSandboxURL};
    _timerFunctionMap.insert(newTimer, timerData);

    newTimer->start(intervalMS);
    return newTimer;
}

QObject* ScriptEngine::setInterval(const QScriptValue& function, int intervalMS) {
    if (_stoppingAllScripts) {
        qCDebug(scriptengine) << "Script.setInterval() while shutting down is ignored... parent script:" << getFilename();
        return NULL; // bail early
    }

    return setupTimerWithInterval(function, intervalMS, false);
}

QObject* ScriptEngine::setTimeout(const QScriptValue& function, int timeoutMS) {
    if (_stoppingAllScripts) {
        qCDebug(scriptengine) << "Script.setTimeout() while shutting down is ignored... parent script:" << getFilename();
        return NULL; // bail early
    }

    return setupTimerWithInterval(function, timeoutMS, true);
}

void ScriptEngine::stopTimer(QTimer *timer) {
    if (_timerFunctionMap.contains(timer)) {
        timer->stop();
        _timerFunctionMap.remove(timer);
        delete timer;
    }
}

QUrl ScriptEngine::resolvePath(const QString& include) const {
    QUrl url(include);
    // first lets check to see if it's already a full URL
    if (!url.scheme().isEmpty()) {
        return expandScriptUrl(url);
    }

    // we apparently weren't a fully qualified url, so, let's assume we're relative
    // to the original URL of our script
    QUrl parentURL;
    if (_parentURL.isEmpty()) {
        parentURL = QUrl(_fileNameString);
    } else {
        parentURL = QUrl(_parentURL);
    }
    // if the parent URL's scheme is empty, then this is probably a local file...
    if (parentURL.scheme().isEmpty()) {
        parentURL = QUrl::fromLocalFile(_fileNameString);
    }

    // at this point we should have a legitimate fully qualified URL for our parent
    url = expandScriptUrl(parentURL.resolved(url));
    return url;
}

void ScriptEngine::print(const QString& message) {
    if (_wantSignals) {
        emit printedMessage(message);
    }
}

// If a callback is specified, the included files will be loaded asynchronously and the callback will be called
// when all of the files have finished loading.
// If no callback is specified, the included files will be loaded synchronously and will block execution until
// all of the files have finished loading.
void ScriptEngine::include(const QStringList& includeFiles, QScriptValue callback) {
    if (_stoppingAllScripts) {
        qCDebug(scriptengine) << "Script.include() while shutting down is ignored..."
        << "includeFiles:" << includeFiles << "parent script:" << getFilename();
        return; // bail early
    }
    QList<QUrl> urls;
    bool knowsSensitivity = false;
    Qt::CaseSensitivity sensitivity;
    auto getSensitivity = [&]() {
        if (!knowsSensitivity) {
            QString path = currentSandboxURL.path();
            QFileInfo upperFI(path.toUpper());
            QFileInfo lowerFI(path.toLower());
            sensitivity = (upperFI == lowerFI) ? Qt::CaseInsensitive : Qt::CaseSensitive;
            knowsSensitivity = true;
        }
        return sensitivity;
    };

    // Guard against meaningless query and fragment parts.
    // Do NOT use PreferLocalFile as its behavior is unpredictable (e.g., on defaultScriptsLocation())
    const auto strippingFlags = QUrl::RemoveFilename | QUrl::RemoveQuery | QUrl::RemoveFragment;
    for (QString file : includeFiles) {
        QUrl thisURL;
        if (file.startsWith("/~/")) {
            thisURL = expandScriptUrl(QUrl::fromLocalFile(expandScriptPath(file)));
            QUrl defaultScriptsLoc = defaultScriptsLocation();
            if (!defaultScriptsLoc.isParentOf(thisURL)) {
                qDebug() << "ScriptEngine::include -- skipping" << file << "-- outside of standard libraries";
                continue;
            }
        } else {
            thisURL = resolvePath(file);
        }

        if (!_includedURLs.contains(thisURL)) {
            if (!currentSandboxURL.isEmpty() && (thisURL.scheme() == "file") &&
                (
                    (currentSandboxURL.scheme() != "file") ||
                        (
                            !thisURL.toString(strippingFlags).startsWith(defaultScriptsLocation().toString(), getSensitivity()) &&
                            !thisURL.toString(strippingFlags).startsWith(currentSandboxURL.toString(strippingFlags), getSensitivity())
                        )
                 )
                ) {
                qCWarning(scriptengine) << "Script.include() ignoring file path" << thisURL << "outside of original entity script" << currentSandboxURL;
            } else {
                // We could also check here for CORS, but we don't yet.
                // It turns out that QUrl.resolve will not change hosts and copy authority, so we don't need to check that here.
                urls.append(thisURL);
                _includedURLs << thisURL;
            }
        } else {
            qCDebug(scriptengine) << "Script.include() ignoring previously included url:" << thisURL;
        }
    }

    BatchLoader* loader = new BatchLoader(urls);
    EntityItemID capturedEntityIdentifier = currentEntityIdentifier;
    QUrl capturedSandboxURL = currentSandboxURL;

    auto evaluateScripts = [=](const QMap<QUrl, QString>& data) {
        auto parentURL = _parentURL;
        for (QUrl url : urls) {
            QString contents = data[url];
            if (contents.isNull()) {
                qCDebug(scriptengine) << "Error loading file: " << url << "line:" << __LINE__;
            } else {
                // Set the parent url so that path resolution will be relative
                // to this script's url during its initial evaluation
                _parentURL = url.toString();
                auto operation = [&]() {
                    evaluate(contents, url.toString());
                };
                doWithEnvironment(capturedEntityIdentifier, capturedSandboxURL, operation);
            }
        }
        _parentURL = parentURL;

        if (callback.isFunction()) {
            callWithEnvironment(capturedEntityIdentifier, capturedSandboxURL, QScriptValue(callback), QScriptValue(), QScriptValueList());
        }

        loader->deleteLater();
    };

    connect(loader, &BatchLoader::finished, this, evaluateScripts);

    // If we are destroyed before the loader completes, make sure to clean it up
    connect(this, &QObject::destroyed, loader, &QObject::deleteLater);

    loader->start();

    if (!callback.isFunction() && !loader->isFinished()) {
        QEventLoop loop;
        QObject::connect(loader, &BatchLoader::finished, &loop, &QEventLoop::quit);
        loop.exec();
    }
}

void ScriptEngine::include(const QString& includeFile, QScriptValue callback) {
    if (_stoppingAllScripts) {
        qCDebug(scriptengine) << "Script.include() while shutting down is ignored... "
            << "includeFile:" << includeFile << "parent script:" << getFilename();
        return; // bail early
    }

    QStringList urls;
    urls.append(includeFile);
    include(urls, callback);
}

// NOTE: The load() command is similar to the include() command except that it loads the script
// as a stand-alone script. To accomplish this, the ScriptEngine class just emits a signal which
// the Application or other context will connect to in order to know to actually load the script
void ScriptEngine::load(const QString& loadFile) {
    if (_stoppingAllScripts) {
        qCDebug(scriptengine) << "Script.load() while shutting down is ignored... "
            << "loadFile:" << loadFile << "parent script:" << getFilename();
        return; // bail early
    }
    if (!currentEntityIdentifier.isInvalidID()) {
        qCWarning(scriptengine) << "Script.load() from entity script is ignored... "
            << "loadFile:" << loadFile << "parent script:" << getFilename();
        return; // bail early
    }

    QUrl url = resolvePath(loadFile);
    if (_isReloading) {
        auto scriptCache = DependencyManager::get<ScriptCache>();
        scriptCache->deleteScript(url.toString());
        if (_wantSignals) {
            emit reloadScript(url.toString(), false);
        }
    } else {
        if (_wantSignals) {
            emit loadScript(url.toString(), false);
        }
    }
}

// Look up the handler associated with eventName and entityID. If found, evalute the argGenerator thunk and call the handler with those args
void ScriptEngine::forwardHandlerCall(const EntityItemID& entityID, const QString& eventName, QScriptValueList eventHandlerArgs) {
    if (QThread::currentThread() != thread()) {
        qDebug() << "*** ERROR *** ScriptEngine::forwardHandlerCall() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]";
        assert(false);
        return ;
    }
    if (!_registeredHandlers.contains(entityID)) {
        return;
    }
    const RegisteredEventHandlers& handlersOnEntity = _registeredHandlers[entityID];
    if (!handlersOnEntity.contains(eventName)) {
        return;
    }
    CallbackList handlersForEvent = handlersOnEntity[eventName];
    if (!handlersForEvent.isEmpty()) {
        for (int i = 0; i < handlersForEvent.count(); ++i) {
            // handlersForEvent[i] can contain many handlers that may have each been added by different interface or entity scripts,
            // and the entity scripts may be for entities other than the one this is a handler for.
            // Fortunately, the definingEntityIdentifier captured the entity script id (if any) when the handler was added.
            CallbackData& handler = handlersForEvent[i];
            callWithEnvironment(handler.definingEntityIdentifier, handler.definingSandboxURL, handler.function, QScriptValue(), eventHandlerArgs);
        }
    }
}

// since all of these operations can be asynch we will always do the actual work in the response handler
// for the download
void ScriptEngine::loadEntityScript(QWeakPointer<ScriptEngine> theEngine, const EntityItemID& entityID, const QString& entityScript, bool forceRedownload) {
    // NOTE: If the script content is not currently in the cache, the LAMBDA here will be called on the Main Thread
    //       which means we're guaranteed that it's not the correct thread for the ScriptEngine. This means
    //       when we get into entityScriptContentAvailable() we will likely invokeMethod() to get it over
    //       to the "Entities" ScriptEngine thread.
    DependencyManager::get<ScriptCache>()->getScriptContents(entityScript, [theEngine, entityID](const QString& scriptOrURL, const QString& contents, bool isURL, bool success) {
        QSharedPointer<ScriptEngine> strongEngine = theEngine.toStrongRef();
        if (strongEngine) {
#ifdef THREAD_DEBUGGING
            qDebug() << "ScriptEngine::entityScriptContentAvailable() IN LAMBDA contentAvailable on thread ["
                << QThread::currentThread() << "] expected thread [" << strongEngine->thread() << "]";
#endif
            strongEngine->entityScriptContentAvailable(entityID, scriptOrURL, contents, isURL, success);
        }
    }, forceRedownload);
}

// since all of these operations can be asynch we will always do the actual work in the response handler
// for the download
void ScriptEngine::entityScriptContentAvailable(const EntityItemID& entityID, const QString& scriptOrURL, const QString& contents, bool isURL, bool success) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::entityScriptContentAvailable() called on wrong thread ["
            << QThread::currentThread() << "], invoking on correct thread [" << thread()
            << "]  " "entityID:" << entityID << "scriptOrURL:" << scriptOrURL << "contents:"
            << contents << "isURL:" << isURL << "success:" << success;
#endif

        QMetaObject::invokeMethod(this, "entityScriptContentAvailable",
                                  Q_ARG(const EntityItemID&, entityID),
                                  Q_ARG(const QString&, scriptOrURL),
                                  Q_ARG(const QString&, contents),
                                  Q_ARG(bool, isURL),
                                  Q_ARG(bool, success));
        return;
    }

#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::entityScriptContentAvailable() thread [" << QThread::currentThread() << "] expected thread [" << thread() << "]";
#endif

    auto scriptCache = DependencyManager::get<ScriptCache>();
    bool isFileUrl = isURL && scriptOrURL.startsWith("file://");
    auto fileName = QString("(EntityID:%1, %2)").arg(entityID.toString(), isURL ? scriptOrURL : "EmbededEntityScript");

    QScriptProgram program(contents, fileName);
    if (!hasCorrectSyntax(program)) {
        if (!isFileUrl) {
            scriptCache->addScriptToBadScriptList(scriptOrURL);
        }
        return; // done processing script
    }

    if (isURL) {
        setParentURL(scriptOrURL);
    }

    QScriptEngine sandbox;
    QScriptValue testConstructor = sandbox.evaluate(program);
    if (hadUncaughtExceptions(sandbox, program.fileName())) {
        return;
    }

    if (!testConstructor.isFunction()) {
        QString testConstructorType = QString(testConstructor.toVariant().typeName());
        if (testConstructorType == "") {
            testConstructorType = "empty";
        }
        QString testConstructorValue = testConstructor.toString();
        const int maxTestConstructorValueSize = 80;
        if (testConstructorValue.size() > maxTestConstructorValueSize) {
            testConstructorValue = testConstructorValue.mid(0, maxTestConstructorValueSize) + "...";
        }
        qCDebug(scriptengine) << "Error -- ScriptEngine::loadEntityScript() entity:" << entityID
                              << "failed to load entity script -- expected a function, got " + testConstructorType
                              << "," << testConstructorValue
                              << "," << scriptOrURL;

        if (!isFileUrl) {
            scriptCache->addScriptToBadScriptList(scriptOrURL);
        }

        return; // done processing script
    }

    int64_t lastModified = 0;
    if (isFileUrl) {
        QString file = QUrl(scriptOrURL).toLocalFile();
        lastModified = (quint64)QFileInfo(file).lastModified().toMSecsSinceEpoch();
    }
    QScriptValue entityScriptConstructor, entityScriptObject;
    QUrl sandboxURL = currentSandboxURL.isEmpty() ? scriptOrURL : currentSandboxURL;
    auto initialization = [&]{
        entityScriptConstructor = evaluate(contents, fileName);
        entityScriptObject = entityScriptConstructor.construct();
    };
    doWithEnvironment(entityID, sandboxURL, initialization);

    EntityScriptDetails newDetails = { scriptOrURL, entityScriptObject, lastModified, sandboxURL };
    _entityScripts[entityID] = newDetails;
    if (isURL) {
        setParentURL("");
    }

    // if we got this far, then call the preload method
    callEntityScriptMethod(entityID, "preload");
}

void ScriptEngine::unloadEntityScript(const EntityItemID& entityID) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::unloadEntityScript() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]  "
            "entityID:" << entityID;
#endif

        QMetaObject::invokeMethod(this, "unloadEntityScript",
                                  Q_ARG(const EntityItemID&, entityID));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::unloadEntityScript() called on correct thread [" << thread() << "]  "
        "entityID:" << entityID;
#endif

    if (_entityScripts.contains(entityID)) {
        callEntityScriptMethod(entityID, "unload");
        _entityScripts.remove(entityID);
        stopAllTimersForEntityScript(entityID);
    }
}

void ScriptEngine::unloadAllEntityScripts() {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::unloadAllEntityScripts() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]";
#endif

        QMetaObject::invokeMethod(this, "unloadAllEntityScripts");
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::unloadAllEntityScripts() called on correct thread [" << thread() << "]";
#endif
    foreach(const EntityItemID& entityID, _entityScripts.keys()) {
        callEntityScriptMethod(entityID, "unload");
    }
    _entityScripts.clear();

#ifdef DEBUG_ENGINE_STATE
    qDebug() << "---- CURRENT STATE OF ENGINE: --------------------------";
    QScriptValueIterator it(globalObject());
    while (it.hasNext()) {
        it.next();
        qDebug() << it.name() << ":" << it.value().toString();
    }
    qDebug() << "--------------------------------------------------------";
#endif // DEBUG_ENGINE_STATE
}

void ScriptEngine::refreshFileScript(const EntityItemID& entityID) {
    if (!_entityScripts.contains(entityID)) {
        return;
    }

    static bool recurseGuard = false;
    if (recurseGuard) {
        return;
    }
    recurseGuard = true;

    EntityScriptDetails details = _entityScripts[entityID];
    // Check to see if a file based script needs to be reloaded (easier debugging)
    if (details.lastModified > 0) {
        QString filePath = QUrl(details.scriptText).toLocalFile();
        auto lastModified = QFileInfo(filePath).lastModified().toMSecsSinceEpoch();
        if (lastModified > details.lastModified) {
            qCDebug(scriptengine) << "Reloading modified script " << details.scriptText;

            QFile file(filePath);
            file.open(QIODevice::ReadOnly);
            QString scriptContents = QTextStream(&file).readAll();
            this->unloadEntityScript(entityID);
            this->entityScriptContentAvailable(entityID, details.scriptText, scriptContents, true, true);
            if (!_entityScripts.contains(entityID)) {
                qWarning() << "Reload script " << details.scriptText << " failed";
            } else {
                details = _entityScripts[entityID];
            }
        }
    }
    recurseGuard = false;
}

// Execute operation in the appropriate context for (the possibly empty) entityID.
// Even if entityID is supplied as currentEntityIdentifier, this still documents the source
// of the code being executed (e.g., if we ever sandbox different entity scripts, or provide different
// global values for different entity scripts).
void ScriptEngine::doWithEnvironment(const EntityItemID& entityID, const QUrl& sandboxURL, std::function<void()> operation) {
    EntityItemID oldIdentifier = currentEntityIdentifier;
    QUrl oldSandboxURL = currentSandboxURL;
    currentEntityIdentifier = entityID;
    currentSandboxURL = sandboxURL;

#if DEBUG_CURRENT_ENTITY
    QScriptValue oldData = this->globalObject().property("debugEntityID");
    this->globalObject().setProperty("debugEntityID", entityID.toScriptValue(this)); // Make the entityID available to javascript as a global.
    operation();
    this->globalObject().setProperty("debugEntityID", oldData);
#else
    operation();
#endif

    currentEntityIdentifier = oldIdentifier;
    currentSandboxURL = oldSandboxURL;
}
void ScriptEngine::callWithEnvironment(const EntityItemID& entityID, const QUrl& sandboxURL, QScriptValue function, QScriptValue thisObject, QScriptValueList args) {
    auto operation = [&]() {
        function.call(thisObject, args);
    };
    doWithEnvironment(entityID, sandboxURL, operation);
}

void ScriptEngine::callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName, const QStringList& params) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::callEntityScriptMethod() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]  "
            "entityID:" << entityID << "methodName:" << methodName;
#endif

        QMetaObject::invokeMethod(this, "callEntityScriptMethod",
                                  Q_ARG(const EntityItemID&, entityID),
                                  Q_ARG(const QString&, methodName),
                                  Q_ARG(const QStringList&, params));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::callEntityScriptMethod() called on correct thread [" << thread() << "]  "
        "entityID:" << entityID << "methodName:" << methodName;
#endif

    refreshFileScript(entityID);
    if (_entityScripts.contains(entityID)) {
        EntityScriptDetails details = _entityScripts[entityID];
        QScriptValue entityScript = details.scriptObject; // previously loaded
        if (entityScript.property(methodName).isFunction()) {
            QScriptValueList args;
            args << entityID.toScriptValue(this);
            args << qScriptValueFromSequence(this, params);
            callWithEnvironment(entityID, details.definingSandboxURL, entityScript.property(methodName), entityScript, args);
        }

    }
}

void ScriptEngine::callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName, const MouseEvent& event) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::callEntityScriptMethod() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]  "
            "entityID:" << entityID << "methodName:" << methodName << "event: mouseEvent";
#endif

        QMetaObject::invokeMethod(this, "callEntityScriptMethod",
                                  Q_ARG(const EntityItemID&, entityID),
                                  Q_ARG(const QString&, methodName),
                                  Q_ARG(const MouseEvent&, event));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::callEntityScriptMethod() called on correct thread [" << thread() << "]  "
        "entityID:" << entityID << "methodName:" << methodName << "event: mouseEvent";
#endif

    refreshFileScript(entityID);
    if (_entityScripts.contains(entityID)) {
        EntityScriptDetails details = _entityScripts[entityID];
        QScriptValue entityScript = details.scriptObject; // previously loaded
        if (entityScript.property(methodName).isFunction()) {
            QScriptValueList args;
            args << entityID.toScriptValue(this);
            args << event.toScriptValue(this);
            callWithEnvironment(entityID, details.definingSandboxURL, entityScript.property(methodName), entityScript, args);
        }
    }
}


void ScriptEngine::callEntityScriptMethod(const EntityItemID& entityID, const QString& methodName, const EntityItemID& otherID, const Collision& collision) {
    if (QThread::currentThread() != thread()) {
#ifdef THREAD_DEBUGGING
        qDebug() << "*** WARNING *** ScriptEngine::callEntityScriptMethod() called on wrong thread [" << QThread::currentThread() << "], invoking on correct thread [" << thread() << "]  "
            "entityID:" << entityID << "methodName:" << methodName << "otherID:" << otherID << "collision: collision";
#endif

        QMetaObject::invokeMethod(this, "callEntityScriptMethod",
                                  Q_ARG(const EntityItemID&, entityID),
                                  Q_ARG(const QString&, methodName),
                                  Q_ARG(const EntityItemID&, otherID),
                                  Q_ARG(const Collision&, collision));
        return;
    }
#ifdef THREAD_DEBUGGING
    qDebug() << "ScriptEngine::callEntityScriptMethod() called on correct thread [" << thread() << "]  "
        "entityID:" << entityID << "methodName:" << methodName << "otherID:" << otherID << "collision: collision";
#endif
    
    refreshFileScript(entityID);
    if (_entityScripts.contains(entityID)) {
        EntityScriptDetails details = _entityScripts[entityID];
        QScriptValue entityScript = details.scriptObject; // previously loaded
        if (entityScript.property(methodName).isFunction()) {
            QScriptValueList args;
            args << entityID.toScriptValue(this);
            args << otherID.toScriptValue(this);
            args << collisionToScriptValue(this, collision);
            callWithEnvironment(entityID, details.definingSandboxURL, entityScript.property(methodName), entityScript, args);
        }
    }
}
