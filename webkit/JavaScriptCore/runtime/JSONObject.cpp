/*
 * Copyright (C) 2009 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. 
 */

#include "config.h"
#include "JSONObject.h"

#include "BooleanObject.h"
#include "Error.h"
#include "ExceptionHelpers.h"
#include "JSArray.h"
#include "JSGlobalObject.h"
#include "LiteralParser.h"
#include "Local.h"
#include "LocalScope.h"
#include "Lookup.h"
#include "ObjectConstructor.h"
#include "JSCInlines.h"
#include "PropertyNameArray.h"
#include <wtf/MathExtras.h>
#include <wtf/text/StringBuilder.h>

namespace JSC {

STATIC_ASSERT_IS_TRIVIALLY_DESTRUCTIBLE(JSONObject);

EncodedJSValue JSC_HOST_CALL JSONProtoFuncParse(ExecState*);
EncodedJSValue JSC_HOST_CALL JSONProtoFuncStringify(ExecState*);

}

#include "JSONObject.lut.h"

namespace JSC {

JSONObject::JSONObject(VM& vm, Structure* structure)
    : JSNonFinalObject(vm, structure)
{
#if PLATFORM(WKC)
    WKC_DEFINE_STATIC_BOOL(inited, false);
    if (!inited) {
        inited = true;
        jsonTable.keys = 0;
    }
#endif
}

void JSONObject::finishCreation(VM& vm)
{
    Base::finishCreation(vm);
    ASSERT(inherits(info()));
}

// PropertyNameForFunctionCall objects must be on the stack, since the JSValue that they create is not marked.
class PropertyNameForFunctionCall {
public:
    PropertyNameForFunctionCall(const Identifier&);
    PropertyNameForFunctionCall(unsigned);

    JSValue value(ExecState*) const;

private:
    const Identifier* m_identifier;
    unsigned m_number;
    mutable JSValue m_value;
};

class Stringifier {
    WTF_MAKE_NONCOPYABLE(Stringifier);
public:
    Stringifier(ExecState*, const Local<Unknown>& replacer, const Local<Unknown>& space);
    Local<Unknown> stringify(Handle<Unknown>);

    void visitAggregate(SlotVisitor&);

private:
    class Holder {
    public:
        Holder(VM&, JSObject*);

        JSObject* object() const { return m_object.get(); }

        bool appendNextProperty(Stringifier&, StringBuilder&);

    private:
        Local<JSObject> m_object;
        const bool m_isArray;
        bool m_isJSArray;
        unsigned m_index;
        unsigned m_size;
        RefPtr<PropertyNameArrayData> m_propertyNames;
    };

    friend class Holder;

    JSValue toJSON(JSValue, const PropertyNameForFunctionCall&);

    enum StringifyResult { StringifyFailed, StringifySucceeded, StringifyFailedDueToUndefinedValue };
    StringifyResult appendStringifiedValue(StringBuilder&, JSValue, JSObject* holder, const PropertyNameForFunctionCall&);

    bool willIndent() const;
    void indent();
    void unindent();
    void startNewLine(StringBuilder&) const;

    ExecState* const m_exec;
    const Local<Unknown> m_replacer;
    bool m_usingArrayReplacer;
    PropertyNameArray m_arrayReplacerPropertyNames;
    CallType m_replacerCallType;
    CallData m_replacerCallData;
    const String m_gap;

    Vector<Holder, 16, UnsafeVectorOverflow> m_holderStack;
    String m_repeatedGap;
    String m_indent;
};

// ------------------------------ helper functions --------------------------------

static inline JSValue unwrapBoxedPrimitive(ExecState* exec, JSValue value)
{
    if (!value.isObject())
        return value;
    JSObject* object = asObject(value);
    if (object->inherits(NumberObject::info()))
        return jsNumber(object->toNumber(exec));
    if (object->inherits(StringObject::info()))
        return object->toString(exec);
    if (object->inherits(BooleanObject::info()))
        return object->toPrimitive(exec);
    return value;
}

static inline String gap(ExecState* exec, JSValue space)
{
    const unsigned maxGapLength = 10;
    space = unwrapBoxedPrimitive(exec, space);

    // If the space value is a number, create a gap string with that number of spaces.
    if (space.isNumber()) {
        double spaceCount = space.asNumber();
        int count;
        if (spaceCount > maxGapLength)
            count = maxGapLength;
        else if (!(spaceCount > 0))
            count = 0;
        else
            count = static_cast<int>(spaceCount);
        UChar spaces[maxGapLength];
        for (int i = 0; i < count; ++i)
            spaces[i] = ' ';
        return String(spaces, count);
    }

    // If the space value is a string, use it as the gap string, otherwise use no gap string.
    String spaces = space.getString(exec);
    if (spaces.length() > maxGapLength) {
        spaces = spaces.substringSharingImpl(0, maxGapLength);
    }
    return spaces;
}

// ------------------------------ PropertyNameForFunctionCall --------------------------------

inline PropertyNameForFunctionCall::PropertyNameForFunctionCall(const Identifier& identifier)
    : m_identifier(&identifier)
{
}

inline PropertyNameForFunctionCall::PropertyNameForFunctionCall(unsigned number)
    : m_identifier(0)
    , m_number(number)
{
}

JSValue PropertyNameForFunctionCall::value(ExecState* exec) const
{
    if (!m_value) {
        if (m_identifier)
            m_value = jsString(exec, m_identifier->string());
        else
            m_value = jsNumber(m_number);
    }
    return m_value;
}

// ------------------------------ Stringifier --------------------------------

Stringifier::Stringifier(ExecState* exec, const Local<Unknown>& replacer, const Local<Unknown>& space)
    : m_exec(exec)
    , m_replacer(replacer)
    , m_usingArrayReplacer(false)
    , m_arrayReplacerPropertyNames(exec)
    , m_replacerCallType(CallTypeNone)
    , m_gap(gap(exec, space.get()))
{
    if (!m_replacer.isObject())
        return;

    if (m_replacer.asObject()->inherits(JSArray::info())) {
        m_usingArrayReplacer = true;
        Handle<JSObject> array = m_replacer.asObject();
        unsigned length = array->get(exec, exec->vm().propertyNames->length).toUInt32(exec);
        for (unsigned i = 0; i < length; ++i) {
            JSValue name = array->get(exec, i);
            if (exec->hadException())
                break;

            if (name.isObject()) {
                if (!asObject(name)->inherits(NumberObject::info()) && !asObject(name)->inherits(StringObject::info()))
                    continue;
            } else if (!name.isNumber() && !name.isString())
                continue;

            m_arrayReplacerPropertyNames.add(name.toString(exec)->toIdentifier(exec));
        }
        return;
    }

    m_replacerCallType = m_replacer.asObject()->methodTable()->getCallData(m_replacer.asObject().get(), m_replacerCallData);
}

Local<Unknown> Stringifier::stringify(Handle<Unknown> value)
{
    JSObject* object = constructEmptyObject(m_exec);
    if (m_exec->hadException())
        return Local<Unknown>(m_exec->vm(), jsNull());

    PropertyNameForFunctionCall emptyPropertyName(m_exec->vm().propertyNames->emptyIdentifier);
    object->putDirect(m_exec->vm(), m_exec->vm().propertyNames->emptyIdentifier, value.get());

    StringBuilder result;
    if (appendStringifiedValue(result, value.get(), object, emptyPropertyName) != StringifySucceeded)
        return Local<Unknown>(m_exec->vm(), jsUndefined());
    if (m_exec->hadException())
        return Local<Unknown>(m_exec->vm(), jsNull());

    return Local<Unknown>(m_exec->vm(), jsString(m_exec, result.toString()));
}

inline JSValue Stringifier::toJSON(JSValue value, const PropertyNameForFunctionCall& propertyName)
{
    ASSERT(!m_exec->hadException());
    if (!value.isObject() || !asObject(value)->hasProperty(m_exec, m_exec->vm().propertyNames->toJSON))
        return value;

    JSValue toJSONFunction = asObject(value)->get(m_exec, m_exec->vm().propertyNames->toJSON);
    if (m_exec->hadException())
        return jsNull();

    if (!toJSONFunction.isObject())
        return value;

    JSObject* object = asObject(toJSONFunction);
    CallData callData;
    CallType callType = object->methodTable()->getCallData(object, callData);
    if (callType == CallTypeNone)
        return value;

    MarkedArgumentBuffer args;
    args.append(propertyName.value(m_exec));
    return call(m_exec, object, callType, callData, value, args);
}

Stringifier::StringifyResult Stringifier::appendStringifiedValue(StringBuilder& builder, JSValue value, JSObject* holder, const PropertyNameForFunctionCall& propertyName)
{
    // Call the toJSON function.
    value = toJSON(value, propertyName);
    if (m_exec->hadException())
        return StringifyFailed;

    // Call the replacer function.
    if (m_replacerCallType != CallTypeNone) {
        MarkedArgumentBuffer args;
        args.append(propertyName.value(m_exec));
        args.append(value);
        value = call(m_exec, m_replacer.get(), m_replacerCallType, m_replacerCallData, holder, args);
        if (m_exec->hadException())
            return StringifyFailed;
    }

    if (value.isUndefined() && !holder->inherits(JSArray::info()))
        return StringifyFailedDueToUndefinedValue;

    if (value.isNull()) {
        builder.appendLiteral("null");
        return StringifySucceeded;
    }

    value = unwrapBoxedPrimitive(m_exec, value);

    if (m_exec->hadException())
        return StringifyFailed;

    if (value.isBoolean()) {
        if (value.isTrue())
            builder.appendLiteral("true");
        else
            builder.appendLiteral("false");
        return StringifySucceeded;
    }

    if (value.isString()) {
        builder.appendQuotedJSONString(asString(value)->value(m_exec));
        return StringifySucceeded;
    }

    if (value.isNumber()) {
        if (value.isInt32())
            builder.appendNumber(value.asInt32());
        else {
            double number = value.asNumber();
            if (!std::isfinite(number))
                builder.appendLiteral("null");
            else
                builder.appendECMAScriptNumber(number);
        }
        return StringifySucceeded;
    }

    if (!value.isObject())
        return StringifyFailed;

    JSObject* object = asObject(value);

    CallData callData;
    if (object->methodTable()->getCallData(object, callData) != CallTypeNone) {
        if (holder->inherits(JSArray::info())) {
            builder.appendLiteral("null");
            return StringifySucceeded;
        }
        return StringifyFailedDueToUndefinedValue;
    }

    // Handle cycle detection, and put the holder on the stack.
    for (unsigned i = 0; i < m_holderStack.size(); i++) {
        if (m_holderStack[i].object() == object) {
            m_exec->vm().throwException(m_exec, createTypeError(m_exec, ASCIILiteral("JSON.stringify cannot serialize cyclic structures.")));
            return StringifyFailed;
        }
    }
    bool holderStackWasEmpty = m_holderStack.isEmpty();
    m_holderStack.append(Holder(m_exec->vm(), object));
    if (!holderStackWasEmpty)
        return StringifySucceeded;

    do {
        while (m_holderStack.last().appendNextProperty(*this, builder)) {
            if (m_exec->hadException())
                return StringifyFailed;
        }
        m_holderStack.removeLast();
    } while (!m_holderStack.isEmpty());
    return StringifySucceeded;
}

inline bool Stringifier::willIndent() const
{
    return !m_gap.isEmpty();
}

inline void Stringifier::indent()
{
    // Use a single shared string, m_repeatedGap, so we don't keep allocating new ones as we indent and unindent.
    unsigned newSize = m_indent.length() + m_gap.length();
    if (newSize > m_repeatedGap.length())
        m_repeatedGap = makeString(m_repeatedGap, m_gap);
    ASSERT(newSize <= m_repeatedGap.length());
    m_indent = m_repeatedGap.substringSharingImpl(0, newSize);
}

inline void Stringifier::unindent()
{
    ASSERT(m_indent.length() >= m_gap.length());
    m_indent = m_repeatedGap.substringSharingImpl(0, m_indent.length() - m_gap.length());
}

inline void Stringifier::startNewLine(StringBuilder& builder) const
{
    if (m_gap.isEmpty())
        return;
    builder.append('\n');
    builder.append(m_indent);
}

inline Stringifier::Holder::Holder(VM& vm, JSObject* object)
    : m_object(vm, object)
    , m_isArray(object->inherits(JSArray::info()))
    , m_index(0)
#ifndef NDEBUG
    , m_size(0)
#endif
{
}

bool Stringifier::Holder::appendNextProperty(Stringifier& stringifier, StringBuilder& builder)
{
    ASSERT(m_index <= m_size);

    ExecState* exec = stringifier.m_exec;

    // First time through, initialize.
    if (!m_index) {
        if (m_isArray) {
            m_isJSArray = isJSArray(m_object.get());
            if (m_isJSArray)
                m_size = asArray(m_object.get())->length();
            else
                m_size = m_object->get(exec, exec->vm().propertyNames->length).toUInt32(exec);
            builder.append('[');
        } else {
            if (stringifier.m_usingArrayReplacer)
                m_propertyNames = stringifier.m_arrayReplacerPropertyNames.data();
            else {
                PropertyNameArray objectPropertyNames(exec);
                m_object->methodTable()->getOwnPropertyNames(m_object.get(), exec, objectPropertyNames, EnumerationMode());
                m_propertyNames = objectPropertyNames.releaseData();
            }
            m_size = m_propertyNames->propertyNameVector().size();
            builder.append('{');
        }
        stringifier.indent();
    }

    // Last time through, finish up and return false.
    if (m_index == m_size) {
        stringifier.unindent();
        if (m_size && builder[builder.length() - 1] != '{')
            stringifier.startNewLine(builder);
        builder.append(m_isArray ? ']' : '}');
        return false;
    }

    // Handle a single element of the array or object.
    unsigned index = m_index++;
    unsigned rollBackPoint = 0;
    StringifyResult stringifyResult;
    if (m_isArray) {
        // Get the value.
        JSValue value;
        if (m_isJSArray && asArray(m_object.get())->canGetIndexQuickly(index))
            value = asArray(m_object.get())->getIndexQuickly(index);
        else {
            PropertySlot slot(m_object.get());
            if (m_object->methodTable()->getOwnPropertySlotByIndex(m_object.get(), exec, index, slot)) {
                value = slot.getValue(exec, index);
                if (exec->hadException())
                    return false;
            } else
                value = jsUndefined();
        }

        // Append the separator string.
        if (index)
            builder.append(',');
        stringifier.startNewLine(builder);

        // Append the stringified value.
        stringifyResult = stringifier.appendStringifiedValue(builder, value, m_object.get(), index);
    } else {
        // Get the value.
        PropertySlot slot(m_object.get());
        Identifier& propertyName = m_propertyNames->propertyNameVector()[index];
        if (!m_object->methodTable()->getOwnPropertySlot(m_object.get(), exec, propertyName, slot))
            return true;
        JSValue value = slot.getValue(exec, propertyName);
        if (exec->hadException())
            return false;

        rollBackPoint = builder.length();

        // Append the separator string.
        if (builder[rollBackPoint - 1] != '{')
            builder.append(',');
        stringifier.startNewLine(builder);

        // Append the property name.
        builder.appendQuotedJSONString(propertyName.string());
        builder.append(':');
        if (stringifier.willIndent())
            builder.append(' ');

        // Append the stringified value.
        stringifyResult = stringifier.appendStringifiedValue(builder, value, m_object.get(), propertyName);
    }

    // From this point on, no access to the this pointer or to any members, because the
    // Holder object may have moved if the call to stringify pushed a new Holder onto
    // m_holderStack.

    switch (stringifyResult) {
        case StringifyFailed:
            builder.appendLiteral("null");
            break;
        case StringifySucceeded:
            break;
        case StringifyFailedDueToUndefinedValue:
            // This only occurs when get an undefined value for an object property.
            // In this case we don't want the separator and property name that we
            // already appended, so roll back.
            builder.resize(rollBackPoint);
            break;
    }

    return true;
}

// ------------------------------ JSONObject --------------------------------

const ClassInfo JSONObject::s_info = { "JSON", &JSNonFinalObject::s_info, &jsonTable, CREATE_METHOD_TABLE(JSONObject) };

/* Source for JSONObject.lut.h
@begin jsonTable
  parse         JSONProtoFuncParse             DontEnum|Function 2
  stringify     JSONProtoFuncStringify         DontEnum|Function 3
@end
*/

// ECMA 15.8

bool JSONObject::getOwnPropertySlot(JSObject* object, ExecState* exec, PropertyName propertyName, PropertySlot& slot)
{
    return getStaticFunctionSlot<JSObject>(exec, jsonTable, jsCast<JSONObject*>(object), propertyName, slot);
}

class Walker {
public:
    Walker(ExecState* exec, Handle<JSObject> function, CallType callType, CallData callData)
        : m_exec(exec)
        , m_function(exec->vm(), function)
        , m_callType(callType)
        , m_callData(callData)
    {
    }
    JSValue walk(JSValue unfiltered);
private:
    JSValue callReviver(JSObject* thisObj, JSValue property, JSValue unfiltered)
    {
        MarkedArgumentBuffer args;
        args.append(property);
        args.append(unfiltered);
        return call(m_exec, m_function.get(), m_callType, m_callData, thisObj, args);
    }

    friend class Holder;

    ExecState* m_exec;
    Local<JSObject> m_function;
    CallType m_callType;
    CallData m_callData;
};

// We clamp recursion well beyond anything reasonable.
static const unsigned maximumFilterRecursion = 40000;
enum WalkerState { StateUnknown, ArrayStartState, ArrayStartVisitMember, ArrayEndVisitMember, 
                                 ObjectStartState, ObjectStartVisitMember, ObjectEndVisitMember };
NEVER_INLINE JSValue Walker::walk(JSValue unfiltered)
{
    Vector<PropertyNameArray, 16, UnsafeVectorOverflow> propertyStack;
    Vector<uint32_t, 16, UnsafeVectorOverflow> indexStack;
    LocalStack<JSObject, 16> objectStack(m_exec->vm());
    LocalStack<JSArray, 16> arrayStack(m_exec->vm());
    
    Vector<WalkerState, 16, UnsafeVectorOverflow> stateStack;
    WalkerState state = StateUnknown;
    JSValue inValue = unfiltered;
    JSValue outValue = jsNull();
    
    while (1) {
        switch (state) {
            arrayStartState:
            case ArrayStartState: {
                ASSERT(inValue.isObject());
                ASSERT(isJSArray(asObject(inValue)) || asObject(inValue)->inherits(JSArray::info()));
                if (objectStack.size() + arrayStack.size() > maximumFilterRecursion)
                    return throwStackOverflowError(m_exec);

                JSArray* array = asArray(inValue);
                arrayStack.push(array);
                indexStack.append(0);
            }
            arrayStartVisitMember:
            FALLTHROUGH;
            case ArrayStartVisitMember: {
                JSArray* array = arrayStack.peek();
                uint32_t index = indexStack.last();
                if (index == array->length()) {
                    outValue = array;
                    arrayStack.pop();
                    indexStack.removeLast();
                    break;
                }
                if (isJSArray(array) && array->canGetIndexQuickly(index))
                    inValue = array->getIndexQuickly(index);
                else {
                    PropertySlot slot(array);
                    if (array->methodTable()->getOwnPropertySlotByIndex(array, m_exec, index, slot))
                        inValue = slot.getValue(m_exec, index);
                    else
                        inValue = jsUndefined();
                }
                    
                if (inValue.isObject()) {
                    stateStack.append(ArrayEndVisitMember);
                    goto stateUnknown;
                } else
                    outValue = inValue;
                FALLTHROUGH;
            }
            case ArrayEndVisitMember: {
                JSArray* array = arrayStack.peek();
                JSValue filteredValue = callReviver(array, jsString(m_exec, String::number(indexStack.last())), outValue);
                if (filteredValue.isUndefined())
                    array->methodTable()->deletePropertyByIndex(array, m_exec, indexStack.last());
                else
                    array->putDirectIndex(m_exec, indexStack.last(), filteredValue);
                if (m_exec->hadException())
                    return jsNull();
                indexStack.last()++;
                goto arrayStartVisitMember;
            }
            objectStartState:
            case ObjectStartState: {
                ASSERT(inValue.isObject());
                ASSERT(!isJSArray(asObject(inValue)) && !asObject(inValue)->inherits(JSArray::info()));
                if (objectStack.size() + arrayStack.size() > maximumFilterRecursion)
                    return throwStackOverflowError(m_exec);

                JSObject* object = asObject(inValue);
                objectStack.push(object);
                indexStack.append(0);
                propertyStack.append(PropertyNameArray(m_exec));
                object->methodTable()->getOwnPropertyNames(object, m_exec, propertyStack.last(), EnumerationMode());
            }
            objectStartVisitMember:
            FALLTHROUGH;
            case ObjectStartVisitMember: {
                JSObject* object = objectStack.peek();
                uint32_t index = indexStack.last();
                PropertyNameArray& properties = propertyStack.last();
                if (index == properties.size()) {
                    outValue = object;
                    objectStack.pop();
                    indexStack.removeLast();
                    propertyStack.removeLast();
                    break;
                }
                PropertySlot slot(object);
                if (object->methodTable()->getOwnPropertySlot(object, m_exec, properties[index], slot))
                    inValue = slot.getValue(m_exec, properties[index]);
                else
                    inValue = jsUndefined();

                // The holder may be modified by the reviver function so any lookup may throw
                if (m_exec->hadException())
                    return jsNull();

                if (inValue.isObject()) {
                    stateStack.append(ObjectEndVisitMember);
                    goto stateUnknown;
                } else
                    outValue = inValue;
                FALLTHROUGH;
            }
            case ObjectEndVisitMember: {
                JSObject* object = objectStack.peek();
                Identifier prop = propertyStack.last()[indexStack.last()];
                PutPropertySlot slot(object);
                JSValue filteredValue = callReviver(object, jsString(m_exec, prop.string()), outValue);
                if (filteredValue.isUndefined())
                    object->methodTable()->deleteProperty(object, m_exec, prop);
                else
                    object->methodTable()->put(object, m_exec, prop, filteredValue, slot);
                if (m_exec->hadException())
                    return jsNull();
                indexStack.last()++;
                goto objectStartVisitMember;
            }
            stateUnknown:
            case StateUnknown:
                if (!inValue.isObject()) {
                    outValue = inValue;
                    break;
                }
                JSObject* object = asObject(inValue);
                if (isJSArray(object) || object->inherits(JSArray::info()))
                    goto arrayStartState;
                goto objectStartState;
        }
        if (stateStack.isEmpty())
            break;

        state = stateStack.last();
        stateStack.removeLast();
    }
    JSObject* finalHolder = constructEmptyObject(m_exec);
    PutPropertySlot slot(finalHolder);
    finalHolder->methodTable()->put(finalHolder, m_exec, m_exec->vm().propertyNames->emptyIdentifier, outValue, slot);
    return callReviver(finalHolder, jsEmptyString(m_exec), outValue);
}

// ECMA-262 v5 15.12.2
EncodedJSValue JSC_HOST_CALL JSONProtoFuncParse(ExecState* exec)
{
    if (!exec->argumentCount())
        return throwVMError(exec, createError(exec, ASCIILiteral("JSON.parse requires at least one parameter")));
    StringView source = exec->uncheckedArgument(0).toString(exec)->view(exec);
    if (exec->hadException())
        return JSValue::encode(jsNull());

    JSValue unfiltered;
    LocalScope scope(exec->vm());
    if (source.is8Bit()) {
        LiteralParser<LChar> jsonParser(exec, source.characters8(), source.length(), StrictJSON);
        unfiltered = jsonParser.tryLiteralParse();
        if (!unfiltered)
            return throwVMError(exec, createSyntaxError(exec, jsonParser.getErrorMessage()));
    } else {
        LiteralParser<UChar> jsonParser(exec, source.characters16(), source.length(), StrictJSON);
        unfiltered = jsonParser.tryLiteralParse();
        if (!unfiltered)
            return throwVMError(exec, createSyntaxError(exec, jsonParser.getErrorMessage()));        
    }
    
    if (exec->argumentCount() < 2)
        return JSValue::encode(unfiltered);
    
    JSValue function = exec->uncheckedArgument(1);
    CallData callData;
    CallType callType = getCallData(function, callData);
    if (callType == CallTypeNone)
        return JSValue::encode(unfiltered);
    return JSValue::encode(Walker(exec, Local<JSObject>(exec->vm(), asObject(function)), callType, callData).walk(unfiltered));
}

// ECMA-262 v5 15.12.3
EncodedJSValue JSC_HOST_CALL JSONProtoFuncStringify(ExecState* exec)
{
    if (!exec->argumentCount())
        return throwVMError(exec, createError(exec, ASCIILiteral("No input to stringify")));
    LocalScope scope(exec->vm());
    Local<Unknown> value(exec->vm(), exec->uncheckedArgument(0));
    Local<Unknown> replacer(exec->vm(), exec->argument(1));
    Local<Unknown> space(exec->vm(), exec->argument(2));
    JSValue result = Stringifier(exec, replacer, space).stringify(value).get();
    return JSValue::encode(result);
}

JSValue JSONParse(ExecState* exec, const String& json)
{
    LocalScope scope(exec->vm());

    if (json.is8Bit()) {
        LiteralParser<LChar> jsonParser(exec, json.characters8(), json.length(), StrictJSON);
        return jsonParser.tryLiteralParse();
    }

    LiteralParser<UChar> jsonParser(exec, json.characters16(), json.length(), StrictJSON);
    return jsonParser.tryLiteralParse();
}

String JSONStringify(ExecState* exec, JSValue value, unsigned indent)
{
    LocalScope scope(exec->vm());
    Local<Unknown> result = Stringifier(exec, Local<Unknown>(exec->vm(), jsNull()), Local<Unknown>(exec->vm(), jsNumber(indent))).stringify(Local<Unknown>(exec->vm(), value));
    if (result.isUndefinedOrNull())
        return String();
    return result.getString(exec);
}

} // namespace JSC