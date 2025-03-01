/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2001 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011 Apple Inc. All
 * rights reserved.
 *           (C) 2006 Alexey Proskuryakov (ap@nypop.com)
 * Copyright (C) 2007 Samuel Weinig (sam@webkit.org)
 * Copyright (C) 2010 Google Inc. All rights reserved.
 * Copyright (C) 2008 Torch Mobile Inc. All rights reserved.
 * (http://www.torchmobile.com/)
 * Copyright (C) 2012 Samsung Electronics. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "core/html/HTMLInputElement.h"

#include "bindings/core/v8/ExceptionMessages.h"
#include "bindings/core/v8/ExceptionState.h"
#include "bindings/core/v8/ScriptEventListener.h"
#include "core/CSSPropertyNames.h"
#include "core/HTMLNames.h"
#include "core/InputTypeNames.h"
#include "core/dom/AXObjectCache.h"
#include "core/dom/Document.h"
#include "core/dom/ExecutionContextTask.h"
#include "core/dom/IdTargetObserver.h"
#include "core/dom/StyleChangeReason.h"
#include "core/dom/shadow/InsertionPoint.h"
#include "core/dom/shadow/ShadowRoot.h"
#include "core/editing/FrameSelection.h"
#include "core/editing/spellcheck/SpellChecker.h"
#include "core/events/BeforeTextInsertedEvent.h"
#include "core/events/KeyboardEvent.h"
#include "core/events/MouseEvent.h"
#include "core/events/ScopedEventQueue.h"
#include "core/frame/Deprecation.h"
#include "core/frame/FrameHost.h"
#include "core/frame/FrameView.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/UseCounter.h"
#include "core/html/HTMLCollection.h"
#include "core/html/HTMLDataListElement.h"
#include "core/html/HTMLDataListOptionsCollection.h"
#include "core/html/HTMLFormElement.h"
#include "core/html/HTMLImageLoader.h"
#include "core/html/HTMLOptionElement.h"
#include "core/html/forms/ColorChooser.h"
#include "core/html/forms/DateTimeChooser.h"
#include "core/html/forms/FileInputType.h"
#include "core/html/forms/FormController.h"
#include "core/html/forms/InputType.h"
#include "core/html/forms/SearchInputType.h"
#include "core/html/parser/HTMLParserIdioms.h"
#include "core/layout/LayoutObject.h"
#include "core/layout/LayoutTheme.h"
#include "core/page/ChromeClient.h"
#include "platform/Language.h"
#include "platform/PlatformMouseEvent.h"
#include "platform/RuntimeEnabledFeatures.h"
#include "platform/text/PlatformLocale.h"
#include "wtf/MathExtras.h"

namespace blink {

using ValueMode = InputType::ValueMode;
using namespace HTMLNames;

class ListAttributeTargetObserver : public IdTargetObserver {
 public:
  static ListAttributeTargetObserver* create(const AtomicString& id,
                                             HTMLInputElement*);
  DECLARE_VIRTUAL_TRACE();
  void idTargetChanged() override;

 private:
  ListAttributeTargetObserver(const AtomicString& id, HTMLInputElement*);

  Member<HTMLInputElement> m_element;
};

const int defaultSize = 20;

HTMLInputElement::HTMLInputElement(Document& document,
                                   HTMLFormElement* form,
                                   bool createdByParser)
    : TextControlElement(inputTag, document, form),
      m_size(defaultSize),
      m_hasDirtyValue(false),
      m_isChecked(false),
      m_dirtyCheckedness(false),
      m_isIndeterminate(false),
      m_isActivatedSubmit(false),
      m_autocomplete(Uninitialized),
      m_hasNonEmptyList(false),
      m_stateRestored(false),
      m_parsingInProgress(createdByParser),
      m_valueAttributeWasUpdatedAfterParsing(false),
      m_canReceiveDroppedFiles(false),
      m_shouldRevealPassword(false),
      m_needsToUpdateViewValue(true),
      m_isPlaceholderVisible(false),
      // |m_inputType| is lazily created when constructed by the parser to avoid
      // constructing unnecessarily a text inputType and its shadow subtree,
      // just to destroy them when the |type| attribute gets set by the parser
      // to something else than 'text'.
      m_inputType(createdByParser ? nullptr : InputType::createText(*this)),
      m_inputTypeView(m_inputType ? m_inputType->createView() : nullptr) {
  setHasCustomStyleCallbacks();
}

HTMLInputElement* HTMLInputElement::create(Document& document,
                                           HTMLFormElement* form,
                                           bool createdByParser) {
  HTMLInputElement* inputElement =
      new HTMLInputElement(document, form, createdByParser);
  if (!createdByParser)
    inputElement->ensureUserAgentShadowRoot();
  return inputElement;
}

DEFINE_TRACE(HTMLInputElement) {
  visitor->trace(m_inputType);
  visitor->trace(m_inputTypeView);
  visitor->trace(m_listAttributeTargetObserver);
  visitor->trace(m_imageLoader);
  TextControlElement::trace(visitor);
}

HTMLImageLoader& HTMLInputElement::ensureImageLoader() {
  if (!m_imageLoader)
    m_imageLoader = HTMLImageLoader::create(this);
  return *m_imageLoader;
}

void HTMLInputElement::didAddUserAgentShadowRoot(ShadowRoot&) {
  m_inputTypeView->createShadowSubtree();
}

HTMLInputElement::~HTMLInputElement() {}

const AtomicString& HTMLInputElement::name() const {
  return m_name.isNull() ? emptyAtom : m_name;
}

Vector<FileChooserFileInfo>
HTMLInputElement::filesFromFileInputFormControlState(
    const FormControlState& state) {
  return FileInputType::filesFromFormControlState(state);
}

bool HTMLInputElement::shouldAutocomplete() const {
  if (m_autocomplete != Uninitialized)
    return m_autocomplete == On;
  return TextControlElement::shouldAutocomplete();
}

bool HTMLInputElement::isValidValue(const String& value) const {
  if (!m_inputType->canSetStringValue()) {
    NOTREACHED();
    return false;
  }
  return !m_inputType->typeMismatchFor(value) &&
         !m_inputType->stepMismatch(value) &&
         !m_inputType->rangeUnderflow(value) &&
         !m_inputType->rangeOverflow(value) &&
         !tooLong(value, IgnoreDirtyFlag) &&
         !tooShort(value, IgnoreDirtyFlag) &&
         !m_inputType->patternMismatch(value) &&
         !m_inputType->valueMissing(value);
}

bool HTMLInputElement::tooLong() const {
  return willValidate() && tooLong(value(), CheckDirtyFlag);
}

bool HTMLInputElement::tooShort() const {
  return willValidate() && tooShort(value(), CheckDirtyFlag);
}

bool HTMLInputElement::typeMismatch() const {
  return willValidate() && m_inputType->typeMismatch();
}

bool HTMLInputElement::valueMissing() const {
  return willValidate() && m_inputType->valueMissing(value());
}

bool HTMLInputElement::hasBadInput() const {
  return willValidate() && m_inputTypeView->hasBadInput();
}

bool HTMLInputElement::patternMismatch() const {
  return willValidate() && m_inputType->patternMismatch(value());
}

bool HTMLInputElement::tooLong(const String& value,
                               NeedsToCheckDirtyFlag check) const {
  return m_inputType->tooLong(value, check);
}

bool HTMLInputElement::tooShort(const String& value,
                                NeedsToCheckDirtyFlag check) const {
  return m_inputType->tooShort(value, check);
}

bool HTMLInputElement::rangeUnderflow() const {
  return willValidate() && m_inputType->rangeUnderflow(value());
}

bool HTMLInputElement::rangeOverflow() const {
  return willValidate() && m_inputType->rangeOverflow(value());
}

String HTMLInputElement::validationMessage() const {
  if (!willValidate())
    return String();

  if (customError())
    return customValidationMessage();

  return m_inputType->validationMessage(*m_inputTypeView).first;
}

String HTMLInputElement::validationSubMessage() const {
  if (!willValidate() || customError())
    return String();
  return m_inputType->validationMessage(*m_inputTypeView).second;
}

double HTMLInputElement::minimum() const {
  return m_inputType->minimum();
}

double HTMLInputElement::maximum() const {
  return m_inputType->maximum();
}

bool HTMLInputElement::stepMismatch() const {
  return willValidate() && m_inputType->stepMismatch(value());
}

bool HTMLInputElement::getAllowedValueStep(Decimal* step) const {
  return m_inputType->getAllowedValueStep(step);
}

StepRange HTMLInputElement::createStepRange(
    AnyStepHandling anyStepHandling) const {
  return m_inputType->createStepRange(anyStepHandling);
}

Decimal HTMLInputElement::findClosestTickMarkValue(const Decimal& value) {
  return m_inputType->findClosestTickMarkValue(value);
}

void HTMLInputElement::stepUp(int n, ExceptionState& exceptionState) {
  m_inputType->stepUp(n, exceptionState);
}

void HTMLInputElement::stepDown(int n, ExceptionState& exceptionState) {
  m_inputType->stepUp(-1.0 * n, exceptionState);
}

void HTMLInputElement::blur() {
  m_inputTypeView->blur();
}

void HTMLInputElement::defaultBlur() {
  TextControlElement::blur();
}

bool HTMLInputElement::hasCustomFocusLogic() const {
  return m_inputTypeView->hasCustomFocusLogic();
}

bool HTMLInputElement::isKeyboardFocusable() const {
  return m_inputType->isKeyboardFocusable();
}

bool HTMLInputElement::shouldShowFocusRingOnMouseFocus() const {
  return m_inputType->shouldShowFocusRingOnMouseFocus();
}

void HTMLInputElement::updateFocusAppearance(
    SelectionBehaviorOnFocus selectionBehavior) {
  if (isTextField()) {
    switch (selectionBehavior) {
      case SelectionBehaviorOnFocus::Reset:
        select();
        break;
      case SelectionBehaviorOnFocus::Restore:
        restoreCachedSelection();
        break;
      case SelectionBehaviorOnFocus::None:
        return;
    }
    // TODO(tkent): scrollRectToVisible is a workaround of a bug of
    // FrameSelection::revealSelection().  It doesn't scroll correctly in a
    // case of RangeSelection. crbug.com/443061.
    if (layoutObject())
      layoutObject()->scrollRectToVisible(boundingBox());
    if (document().frame())
      document().frame()->selection().revealSelection();
  } else {
    TextControlElement::updateFocusAppearance(selectionBehavior);
  }
}

void HTMLInputElement::beginEditing() {
  DCHECK(document().isActive());
  if (!document().isActive())
    return;

  if (!isTextField())
    return;

  document().frame()->spellChecker().didBeginEditing(this);
}

void HTMLInputElement::endEditing() {
  DCHECK(document().isActive());
  if (!document().isActive())
    return;

  if (!isTextField())
    return;

  LocalFrame* frame = document().frame();
  frame->spellChecker().didEndEditingOnTextField(this);
  frame->host()->chromeClient().didEndEditingOnTextField(*this);
}

void HTMLInputElement::handleFocusEvent(Element* oldFocusedElement,
                                        WebFocusType type) {
  m_inputTypeView->handleFocusEvent(oldFocusedElement, type);
  m_inputType->enableSecureTextInput();
}

void HTMLInputElement::dispatchFocusInEvent(
    const AtomicString& eventType,
    Element* oldFocusedElement,
    WebFocusType type,
    InputDeviceCapabilities* sourceCapabilities) {
  if (eventType == EventTypeNames::DOMFocusIn)
    m_inputTypeView->handleFocusInEvent(oldFocusedElement, type);
  HTMLFormControlElementWithState::dispatchFocusInEvent(
      eventType, oldFocusedElement, type, sourceCapabilities);
}

void HTMLInputElement::handleBlurEvent() {
  m_inputType->disableSecureTextInput();
  m_inputTypeView->handleBlurEvent();
}

void HTMLInputElement::setType(const AtomicString& type) {
  setAttribute(typeAttr, type);
}

void HTMLInputElement::initializeTypeInParsing() {
  DCHECK(m_parsingInProgress);
  DCHECK(!m_inputType);
  DCHECK(!m_inputTypeView);

  const AtomicString& newTypeName =
      InputType::normalizeTypeName(fastGetAttribute(typeAttr));
  m_inputType = InputType::create(*this, newTypeName);
  m_inputTypeView = m_inputType->createView();
  String defaultValue = fastGetAttribute(valueAttr);
  if (m_inputType->valueMode() == ValueMode::kValue)
    m_nonAttributeValue = sanitizeValue(defaultValue);
  ensureUserAgentShadowRoot();

  setNeedsWillValidateCheck();

  if (!defaultValue.isNull())
    m_inputType->warnIfValueIsInvalid(defaultValue);

  m_inputTypeView->updateView();
  setTextAsOfLastFormControlChangeEvent(value());
  setChangedSinceLastFormControlChangeEvent(false);
}

void HTMLInputElement::updateType() {
  DCHECK(m_inputType);
  DCHECK(m_inputTypeView);

  const AtomicString& newTypeName =
      InputType::normalizeTypeName(fastGetAttribute(typeAttr));
  if (m_inputType->formControlType() == newTypeName)
    return;

  InputType* newType = InputType::create(*this, newTypeName);
  removeFromRadioButtonGroup();

  ValueMode oldValueMode = m_inputType->valueMode();
  bool didRespectHeightAndWidth =
      m_inputType->shouldRespectHeightAndWidthAttributes();
  bool couldBeSuccessfulSubmitButton = canBeSuccessfulSubmitButton();

  m_inputTypeView->destroyShadowSubtree();
  lazyReattachIfAttached();

  m_inputType = newType;
  m_inputTypeView = m_inputType->createView();
  m_inputTypeView->createShadowSubtree();

  setNeedsWillValidateCheck();

  ValueMode newValueMode = m_inputType->valueMode();

  // https://html.spec.whatwg.org/multipage/forms.html#input-type-change
  //
  // 1. If the previous state of the element's type attribute put the value IDL
  // attribute in the value mode, and the element's value is not the empty
  // string, and the new state of the element's type attribute puts the value
  // IDL attribute in either the default mode or the default/on mode, then set
  // the element's value content attribute to the element's value.
  if (oldValueMode == ValueMode::kValue &&
      (newValueMode == ValueMode::kDefault ||
       newValueMode == ValueMode::kDefaultOn)) {
    if (hasDirtyValue())
      setAttribute(valueAttr, AtomicString(m_nonAttributeValue));
    m_nonAttributeValue = String();
    m_hasDirtyValue = false;
  }
  // 2. Otherwise, if the previous state of the element's type attribute put the
  // value IDL attribute in any mode other than the value mode, and the new
  // state of the element's type attribute puts the value IDL attribute in the
  // value mode, then set the value of the element to the value of the value
  // content attribute, if there is one, or the empty string otherwise, and then
  // set the control's dirty value flag to false.
  else if (oldValueMode != ValueMode::kValue &&
           newValueMode == ValueMode::kValue) {
    AtomicString valueString = fastGetAttribute(valueAttr);
    m_inputType->warnIfValueIsInvalid(valueString);
    m_nonAttributeValue = sanitizeValue(valueString);
    m_hasDirtyValue = false;
  }
  // 3. Otherwise, if the previous state of the element's type attribute put the
  // value IDL attribute in any mode other than the filename mode, and the new
  // state of the element's type attribute puts the value IDL attribute in the
  // filename mode, then set the value of the element to the empty string.
  else if (oldValueMode != ValueMode::kFilename &&
           newValueMode == ValueMode::kFilename) {
    m_nonAttributeValue = String();
    m_hasDirtyValue = false;

  } else {
    // ValueMode wasn't changed, or kDefault <-> kDefaultOn.
    if (!hasDirtyValue()) {
      String defaultValue = fastGetAttribute(valueAttr);
      if (!defaultValue.isNull())
        m_inputType->warnIfValueIsInvalid(defaultValue);
    }

    if (newValueMode == ValueMode::kValue) {
      String newValue = sanitizeValue(m_nonAttributeValue);
      if (!equalIgnoringNullity(newValue, m_nonAttributeValue)) {
        if (hasDirtyValue())
          setValue(newValue);
        else
          setNonDirtyValue(newValue);
      }
    }
  }

  m_needsToUpdateViewValue = true;
  m_inputTypeView->updateView();

  if (didRespectHeightAndWidth !=
      m_inputType->shouldRespectHeightAndWidthAttributes()) {
    DCHECK(elementData());
    AttributeCollection attributes = attributesWithoutUpdate();
    if (const Attribute* height = attributes.find(heightAttr)) {
      TextControlElement::attributeChanged(heightAttr, height->value(),
                                           height->value());
    }
    if (const Attribute* width = attributes.find(widthAttr)) {
      TextControlElement::attributeChanged(widthAttr, width->value(),
                                           width->value());
    }
    if (const Attribute* align = attributes.find(alignAttr)) {
      TextControlElement::attributeChanged(alignAttr, align->value(),
                                           align->value());
    }
  }

  if (document().focusedElement() == this)
    document().updateFocusAppearanceSoon(SelectionBehaviorOnFocus::Restore);

  setTextAsOfLastFormControlChangeEvent(value());
  setChangedSinceLastFormControlChangeEvent(false);

  addToRadioButtonGroup();

  setNeedsValidityCheck();
  if ((couldBeSuccessfulSubmitButton || canBeSuccessfulSubmitButton()) &&
      formOwner() && isConnected())
    formOwner()->invalidateDefaultButtonStyle();
  notifyFormStateChanged();
}

void HTMLInputElement::subtreeHasChanged() {
  m_inputTypeView->subtreeHasChanged();
  // When typing in an input field, childrenChanged is not called, so we need to
  // force the directionality check.
  calculateAndAdjustDirectionality();
}

const AtomicString& HTMLInputElement::formControlType() const {
  return m_inputType->formControlType();
}

bool HTMLInputElement::shouldSaveAndRestoreFormControlState() const {
  if (!m_inputType->shouldSaveAndRestoreFormControlState())
    return false;
  return TextControlElement::shouldSaveAndRestoreFormControlState();
}

FormControlState HTMLInputElement::saveFormControlState() const {
  return m_inputTypeView->saveFormControlState();
}

void HTMLInputElement::restoreFormControlState(const FormControlState& state) {
  m_inputTypeView->restoreFormControlState(state);
  m_stateRestored = true;
}

bool HTMLInputElement::canStartSelection() const {
  if (!isTextField())
    return false;
  return TextControlElement::canStartSelection();
}

int HTMLInputElement::selectionStartForBinding(
    ExceptionState& exceptionState) const {
  if (!m_inputType->supportsSelectionAPI()) {
    UseCounter::count(document(), UseCounter::InputSelectionGettersThrow);
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return 0;
  }
  return TextControlElement::selectionStart();
}

int HTMLInputElement::selectionEndForBinding(
    ExceptionState& exceptionState) const {
  if (!m_inputType->supportsSelectionAPI()) {
    UseCounter::count(document(), UseCounter::InputSelectionGettersThrow);
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return 0;
  }
  return TextControlElement::selectionEnd();
}

String HTMLInputElement::selectionDirectionForBinding(
    ExceptionState& exceptionState) const {
  if (!m_inputType->supportsSelectionAPI()) {
    UseCounter::count(document(), UseCounter::InputSelectionGettersThrow);
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return String();
  }
  return TextControlElement::selectionDirection();
}

void HTMLInputElement::setSelectionStartForBinding(
    int start,
    ExceptionState& exceptionState) {
  if (!m_inputType->supportsSelectionAPI()) {
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return;
  }
  TextControlElement::setSelectionStart(start);
}

void HTMLInputElement::setSelectionEndForBinding(
    int end,
    ExceptionState& exceptionState) {
  if (!m_inputType->supportsSelectionAPI()) {
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return;
  }
  TextControlElement::setSelectionEnd(end);
}

void HTMLInputElement::setSelectionDirectionForBinding(
    const String& direction,
    ExceptionState& exceptionState) {
  if (!m_inputType->supportsSelectionAPI()) {
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return;
  }
  TextControlElement::setSelectionDirection(direction);
}

void HTMLInputElement::setSelectionRangeForBinding(
    int start,
    int end,
    ExceptionState& exceptionState) {
  if (!m_inputType->supportsSelectionAPI()) {
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return;
  }
  TextControlElement::setSelectionRangeForBinding(start, end);
}

void HTMLInputElement::setSelectionRangeForBinding(
    int start,
    int end,
    const String& direction,
    ExceptionState& exceptionState) {
  if (!m_inputType->supportsSelectionAPI()) {
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return;
  }
  TextControlElement::setSelectionRangeForBinding(start, end, direction);
}

void HTMLInputElement::accessKeyAction(bool sendMouseEvents) {
  m_inputTypeView->accessKeyAction(sendMouseEvents);
}

bool HTMLInputElement::isPresentationAttribute(
    const QualifiedName& name) const {
  // FIXME: Remove type check.
  if (name == vspaceAttr || name == hspaceAttr || name == alignAttr ||
      name == widthAttr || name == heightAttr ||
      (name == borderAttr && type() == InputTypeNames::image))
    return true;
  return TextControlElement::isPresentationAttribute(name);
}

void HTMLInputElement::collectStyleForPresentationAttribute(
    const QualifiedName& name,
    const AtomicString& value,
    MutableStylePropertySet* style) {
  if (name == vspaceAttr) {
    addHTMLLengthToStyle(style, CSSPropertyMarginTop, value);
    addHTMLLengthToStyle(style, CSSPropertyMarginBottom, value);
  } else if (name == hspaceAttr) {
    addHTMLLengthToStyle(style, CSSPropertyMarginLeft, value);
    addHTMLLengthToStyle(style, CSSPropertyMarginRight, value);
  } else if (name == alignAttr) {
    if (m_inputType->shouldRespectAlignAttribute())
      applyAlignmentAttributeToStyle(value, style);
  } else if (name == widthAttr) {
    if (m_inputType->shouldRespectHeightAndWidthAttributes())
      addHTMLLengthToStyle(style, CSSPropertyWidth, value);
  } else if (name == heightAttr) {
    if (m_inputType->shouldRespectHeightAndWidthAttributes())
      addHTMLLengthToStyle(style, CSSPropertyHeight, value);
  } else if (name == borderAttr &&
             type() == InputTypeNames::image) {  // FIXME: Remove type check.
    applyBorderAttributeToStyle(value, style);
  } else {
    TextControlElement::collectStyleForPresentationAttribute(name, value,
                                                             style);
  }
}

void HTMLInputElement::parseAttribute(const QualifiedName& name,
                                      const AtomicString& oldValue,
                                      const AtomicString& value) {
  DCHECK(m_inputType);
  DCHECK(m_inputTypeView);

  if (name == nameAttr) {
    removeFromRadioButtonGroup();
    m_name = value;
    addToRadioButtonGroup();
    TextControlElement::parseAttribute(name, oldValue, value);
  } else if (name == autocompleteAttr) {
    if (equalIgnoringCase(value, "off")) {
      m_autocomplete = Off;
    } else {
      if (value.isEmpty())
        m_autocomplete = Uninitialized;
      else
        m_autocomplete = On;
    }
  } else if (name == typeAttr) {
    updateType();
  } else if (name == valueAttr) {
    // We only need to setChanged if the form is looking at the default value
    // right now.
    if (!hasDirtyValue()) {
      if (m_inputType->valueMode() == ValueMode::kValue) {
        m_nonAttributeValue = sanitizeValue(value);
        setTextAsOfLastFormControlChangeEvent(m_nonAttributeValue);
      }
      updatePlaceholderVisibility();
      setNeedsStyleRecalc(
          SubtreeStyleChange,
          StyleChangeReasonForTracing::fromAttribute(valueAttr));
    }
    m_needsToUpdateViewValue = true;
    setNeedsValidityCheck();
    m_valueAttributeWasUpdatedAfterParsing = !m_parsingInProgress;
    m_inputType->warnIfValueIsInvalidAndElementIsVisible(value);
    m_inputTypeView->valueAttributeChanged();
  } else if (name == checkedAttr) {
    // Another radio button in the same group might be checked by state
    // restore. We shouldn't call setChecked() even if this has the checked
    // attribute. So, delay the setChecked() call until
    // finishParsingChildren() is called if parsing is in progress.
    if ((!m_parsingInProgress ||
         !document().formController().hasFormStates()) &&
        !m_dirtyCheckedness) {
      setChecked(!value.isNull());
      m_dirtyCheckedness = false;
    }
    pseudoStateChanged(CSSSelector::PseudoDefault);
  } else if (name == maxlengthAttr) {
    setNeedsValidityCheck();
  } else if (name == minlengthAttr) {
    setNeedsValidityCheck();
  } else if (name == sizeAttr) {
    int oldSize = m_size;
    m_size = defaultSize;
    int valueAsInteger;
    if (!value.isEmpty() && parseHTMLInteger(value, valueAsInteger) &&
        valueAsInteger > 0)
      m_size = valueAsInteger;
    if (m_size != oldSize && layoutObject())
      layoutObject()->setNeedsLayoutAndPrefWidthsRecalcAndFullPaintInvalidation(
          LayoutInvalidationReason::AttributeChanged);
  } else if (name == altAttr) {
    m_inputTypeView->altAttributeChanged();
  } else if (name == srcAttr) {
    m_inputTypeView->srcAttributeChanged();
  } else if (name == usemapAttr || name == accesskeyAttr) {
    // FIXME: ignore for the moment
  } else if (name == onsearchAttr) {
    // Search field and slider attributes all just cause updateFromElement to be
    // called through style recalcing.
    setAttributeEventListener(
        EventTypeNames::search,
        createAttributeEventListener(this, name, value, eventParameterName()));
  } else if (name == incrementalAttr) {
    UseCounter::count(document(), UseCounter::IncrementalAttribute);
  } else if (name == minAttr) {
    m_inputTypeView->minOrMaxAttributeChanged();
    m_inputType->sanitizeValueInResponseToMinOrMaxAttributeChange();
    setNeedsValidityCheck();
    UseCounter::count(document(), UseCounter::MinAttribute);
  } else if (name == maxAttr) {
    m_inputTypeView->minOrMaxAttributeChanged();
    m_inputType->sanitizeValueInResponseToMinOrMaxAttributeChange();
    setNeedsValidityCheck();
    UseCounter::count(document(), UseCounter::MaxAttribute);
  } else if (name == multipleAttr) {
    m_inputTypeView->multipleAttributeChanged();
    setNeedsValidityCheck();
  } else if (name == stepAttr) {
    m_inputTypeView->stepAttributeChanged();
    setNeedsValidityCheck();
    UseCounter::count(document(), UseCounter::StepAttribute);
  } else if (name == patternAttr) {
    setNeedsValidityCheck();
    UseCounter::count(document(), UseCounter::PatternAttribute);
  } else if (name == readonlyAttr) {
    TextControlElement::parseAttribute(name, oldValue, value);
    m_inputTypeView->readonlyAttributeChanged();
  } else if (name == listAttr) {
    m_hasNonEmptyList = !value.isEmpty();
    if (m_hasNonEmptyList) {
      resetListAttributeTargetObserver();
      listAttributeTargetChanged();
    }
    UseCounter::count(document(), UseCounter::ListAttribute);
  } else if (name == webkitdirectoryAttr) {
    TextControlElement::parseAttribute(name, oldValue, value);
    UseCounter::count(document(), UseCounter::PrefixedDirectoryAttribute);
  } else {
    if (name == formactionAttr)
      logUpdateAttributeIfIsolatedWorldAndInDocument("input", formactionAttr,
                                                     oldValue, value);
    TextControlElement::parseAttribute(name, oldValue, value);
  }
  m_inputTypeView->attributeChanged();
}

void HTMLInputElement::parserDidSetAttributes() {
  DCHECK(m_parsingInProgress);
  initializeTypeInParsing();
}

void HTMLInputElement::finishParsingChildren() {
  m_parsingInProgress = false;
  DCHECK(m_inputType);
  DCHECK(m_inputTypeView);
  TextControlElement::finishParsingChildren();
  if (!m_stateRestored) {
    bool checked = hasAttribute(checkedAttr);
    if (checked)
      setChecked(checked);
    m_dirtyCheckedness = false;
  }
}

bool HTMLInputElement::layoutObjectIsNeeded(const ComputedStyle& style) {
  return m_inputType->layoutObjectIsNeeded() &&
         TextControlElement::layoutObjectIsNeeded(style);
}

LayoutObject* HTMLInputElement::createLayoutObject(const ComputedStyle& style) {
  return m_inputTypeView->createLayoutObject(style);
}

void HTMLInputElement::attachLayoutTree(const AttachContext& context) {
  TextControlElement::attachLayoutTree(context);
  if (layoutObject()) {
    m_inputType->onAttachWithLayoutObject();
  }

  m_inputTypeView->startResourceLoading();
  m_inputType->countUsage();

  if (document().focusedElement() == this)
    document().updateFocusAppearanceSoon(SelectionBehaviorOnFocus::Restore);
}

void HTMLInputElement::detachLayoutTree(const AttachContext& context) {
  if (layoutObject()) {
    m_inputType->onDetachWithLayoutObject();
  }
  TextControlElement::detachLayoutTree(context);
  m_needsToUpdateViewValue = true;
  m_inputTypeView->closePopupView();
}

String HTMLInputElement::altText() const {
  // http://www.w3.org/TR/1998/REC-html40-19980424/appendix/notes.html#altgen
  // also heavily discussed by Hixie on bugzilla
  // note this is intentionally different to HTMLImageElement::altText()
  String alt = fastGetAttribute(altAttr);
  // fall back to title attribute
  if (alt.isNull())
    alt = fastGetAttribute(titleAttr);
  if (alt.isNull())
    alt = fastGetAttribute(valueAttr);
  if (alt.isNull())
    alt = locale().queryString(WebLocalizedString::InputElementAltText);
  return alt;
}

bool HTMLInputElement::canBeSuccessfulSubmitButton() const {
  return m_inputType->canBeSuccessfulSubmitButton();
}

bool HTMLInputElement::isActivatedSubmit() const {
  return m_isActivatedSubmit;
}

void HTMLInputElement::setActivatedSubmit(bool flag) {
  m_isActivatedSubmit = flag;
}

void HTMLInputElement::appendToFormData(FormData& formData) {
  if (m_inputType->isFormDataAppendable())
    m_inputType->appendToFormData(formData);
}

String HTMLInputElement::resultForDialogSubmit() {
  return m_inputType->resultForDialogSubmit();
}

void HTMLInputElement::resetImpl() {
  if (m_inputType->valueMode() == ValueMode::kValue) {
    setNonDirtyValue(defaultValue());
    setNeedsValidityCheck();
  } else if (m_inputType->valueMode() == ValueMode::kFilename) {
    setNonDirtyValue(String());
    setNeedsValidityCheck();
  }

  setChecked(hasAttribute(checkedAttr));
  m_dirtyCheckedness = false;
}

bool HTMLInputElement::isTextField() const {
  return m_inputType->isTextField();
}

void HTMLInputElement::dispatchChangeEventIfNeeded() {
  if (isConnected() && m_inputType->shouldSendChangeEventAfterCheckedChanged())
    dispatchChangeEvent();
}

bool HTMLInputElement::checked() const {
  m_inputType->readingChecked();
  return m_isChecked;
}

void HTMLInputElement::setChecked(bool nowChecked,
                                  TextFieldEventBehavior eventBehavior) {
  m_dirtyCheckedness = true;
  if (checked() == nowChecked)
    return;

  m_isChecked = nowChecked;

  if (RadioButtonGroupScope* scope = radioButtonGroupScope())
    scope->updateCheckedState(this);
  if (layoutObject())
    LayoutTheme::theme().controlStateChanged(*layoutObject(),
                                             CheckedControlState);

  setNeedsValidityCheck();

  // Ideally we'd do this from the layout tree (matching
  // LayoutTextView), but it's not possible to do it at the moment
  // because of the way the code is structured.
  if (layoutObject()) {
    if (AXObjectCache* cache =
            layoutObject()->document().existingAXObjectCache())
      cache->checkedStateChanged(this);
  }

  // Only send a change event for items in the document (avoid firing during
  // parsing) and don't send a change event for a radio button that's getting
  // unchecked to match other browsers. DOM is not a useful standard for this
  // because it says only to fire change events at "lose focus" time, which is
  // definitely wrong in practice for these types of elements.
  if (eventBehavior != DispatchNoEvent && isConnected() &&
      m_inputType->shouldSendChangeEventAfterCheckedChanged()) {
    setTextAsOfLastFormControlChangeEvent(String());
    if (eventBehavior == DispatchInputAndChangeEvent)
      dispatchFormControlInputEvent();
  }

  pseudoStateChanged(CSSSelector::PseudoChecked);
}

void HTMLInputElement::setIndeterminate(bool newValue) {
  if (indeterminate() == newValue)
    return;

  m_isIndeterminate = newValue;

  pseudoStateChanged(CSSSelector::PseudoIndeterminate);

  if (layoutObject())
    LayoutTheme::theme().controlStateChanged(*layoutObject(),
                                             CheckedControlState);
}

int HTMLInputElement::size() const {
  return m_size;
}

bool HTMLInputElement::sizeShouldIncludeDecoration(int& preferredSize) const {
  return m_inputTypeView->sizeShouldIncludeDecoration(defaultSize,
                                                      preferredSize);
}

void HTMLInputElement::copyNonAttributePropertiesFromElement(
    const Element& source) {
  const HTMLInputElement& sourceElement =
      static_cast<const HTMLInputElement&>(source);

  m_nonAttributeValue = sourceElement.m_nonAttributeValue;
  m_hasDirtyValue = sourceElement.m_hasDirtyValue;
  setChecked(sourceElement.m_isChecked);
  m_dirtyCheckedness = sourceElement.m_dirtyCheckedness;
  m_isIndeterminate = sourceElement.m_isIndeterminate;
  m_inputType->copyNonAttributeProperties(sourceElement);

  TextControlElement::copyNonAttributePropertiesFromElement(source);

  m_needsToUpdateViewValue = true;
  m_inputTypeView->updateView();
}

String HTMLInputElement::value() const {
  switch (m_inputType->valueMode()) {
    case ValueMode::kFilename:
      return m_inputType->valueInFilenameValueMode();
    case ValueMode::kDefault:
      return fastGetAttribute(valueAttr);
    case ValueMode::kDefaultOn: {
      AtomicString valueString = fastGetAttribute(valueAttr);
      return valueString.isNull() ? "on" : valueString;
    }
    case ValueMode::kValue:
      return m_nonAttributeValue;
  }
  NOTREACHED();
  return emptyString();
}

String HTMLInputElement::valueOrDefaultLabel() const {
  String value = this->value();
  if (!value.isNull())
    return value;
  return m_inputType->defaultLabel();
}

void HTMLInputElement::setValueForUser(const String& value) {
  // Call setValue and make it send a change event.
  setValue(value, DispatchChangeEvent);
}

const String& HTMLInputElement::suggestedValue() const {
  return m_suggestedValue;
}

void HTMLInputElement::setSuggestedValue(const String& value) {
  if (!m_inputType->canSetSuggestedValue())
    return;
  m_needsToUpdateViewValue = true;
  m_suggestedValue = sanitizeValue(value);
  setNeedsStyleRecalc(SubtreeStyleChange, StyleChangeReasonForTracing::create(
                                              StyleChangeReason::ControlValue));
  m_inputTypeView->updateView();
}

void HTMLInputElement::setEditingValue(const String& value) {
  if (!layoutObject() || !isTextField())
    return;
  setInnerEditorValue(value);
  subtreeHasChanged();

  unsigned max = value.length();
  setSelectionRange(max, max);
  dispatchInputEvent();
}

void HTMLInputElement::setInnerEditorValue(const String& value) {
  TextControlElement::setInnerEditorValue(value);
  m_needsToUpdateViewValue = false;
}

void HTMLInputElement::setValue(const String& value,
                                ExceptionState& exceptionState,
                                TextFieldEventBehavior eventBehavior) {
  // FIXME: Remove type check.
  if (type() == InputTypeNames::file && !value.isEmpty()) {
    exceptionState.throwDOMException(InvalidStateError,
                                     "This input element accepts a filename, "
                                     "which may only be programmatically set "
                                     "to the empty string.");
    return;
  }
  setValue(value, eventBehavior);
}

void HTMLInputElement::setValue(const String& value,
                                TextFieldEventBehavior eventBehavior) {
  m_inputType->warnIfValueIsInvalidAndElementIsVisible(value);
  if (!m_inputType->canSetValue(value))
    return;

  EventQueueScope scope;
  String sanitizedValue = sanitizeValue(value);
  bool valueChanged = sanitizedValue != this->value();

  setLastChangeWasNotUserEdit();
  m_needsToUpdateViewValue = true;
  // Prevent TextFieldInputType::setValue from using the suggested value.
  m_suggestedValue = String();

  m_inputType->setValue(sanitizedValue, valueChanged, eventBehavior);
  m_inputTypeView->didSetValue(sanitizedValue, valueChanged);

  if (valueChanged)
    notifyFormStateChanged();
}

void HTMLInputElement::setNonAttributeValue(const String& sanitizedValue) {
  // This is a common code for ValueMode::kValue.
  DCHECK_EQ(m_inputType->valueMode(), ValueMode::kValue);
  m_nonAttributeValue = sanitizedValue;
  m_hasDirtyValue = true;
  setNeedsValidityCheck();
  if (m_inputType->isSteppable()) {
    pseudoStateChanged(CSSSelector::PseudoInRange);
    pseudoStateChanged(CSSSelector::PseudoOutOfRange);
  }
  if (document().focusedElement() == this)
    document()
        .frameHost()
        ->chromeClient()
        .didUpdateTextOfFocusedElementByNonUserInput(*document().frame());
}

void HTMLInputElement::setNonDirtyValue(const String& newValue) {
  setValue(newValue);
  m_hasDirtyValue = false;
}

bool HTMLInputElement::hasDirtyValue() const {
  return m_hasDirtyValue;
}

void HTMLInputElement::updateView() {
  m_inputTypeView->updateView();
}

double HTMLInputElement::valueAsDate(bool& isNull) const {
  double date = m_inputType->valueAsDate();
  isNull = !std::isfinite(date);
  return date;
}

void HTMLInputElement::setValueAsDate(double value,
                                      ExceptionState& exceptionState) {
  m_inputType->setValueAsDate(value, exceptionState);
}

double HTMLInputElement::valueAsNumber() const {
  return m_inputType->valueAsDouble();
}

void HTMLInputElement::setValueAsNumber(double newValue,
                                        ExceptionState& exceptionState,
                                        TextFieldEventBehavior eventBehavior) {
  // http://www.whatwg.org/specs/web-apps/current-work/multipage/common-input-element-attributes.html#dom-input-valueasnumber
  // On setting, if the new value is infinite, then throw a TypeError exception.
  if (std::isinf(newValue)) {
    exceptionState.throwTypeError(
        ExceptionMessages::notAFiniteNumber(newValue));
    return;
  }
  m_inputType->setValueAsDouble(newValue, eventBehavior, exceptionState);
}

void HTMLInputElement::setValueFromRenderer(const String& value) {
  // File upload controls will never use this.
  DCHECK_NE(type(), InputTypeNames::file);

  m_suggestedValue = String();

  // Renderer and our event handler are responsible for sanitizing values.
  DCHECK(value == m_inputType->sanitizeUserInputValue(value) ||
         m_inputType->sanitizeUserInputValue(value).isEmpty());

  DCHECK(!value.isNull());
  m_nonAttributeValue = value;
  m_hasDirtyValue = true;
  m_needsToUpdateViewValue = false;

  // Input event is fired by the Node::defaultEventHandler for editable
  // controls.
  if (!isTextField())
    dispatchInputEvent();
  notifyFormStateChanged();

  setNeedsValidityCheck();

  // Clear autofill flag (and yellow background) on user edit.
  setAutofilled(false);
}

EventDispatchHandlingState* HTMLInputElement::preDispatchEventHandler(
    Event* event) {
  if (event->type() == EventTypeNames::textInput &&
      m_inputTypeView->shouldSubmitImplicitly(event)) {
    event->stopPropagation();
    return nullptr;
  }
  if (event->type() != EventTypeNames::click)
    return nullptr;
  if (!event->isMouseEvent() ||
      toMouseEvent(event)->button() !=
          static_cast<short>(WebPointerProperties::Button::Left))
    return nullptr;
  return m_inputTypeView->willDispatchClick();
}

void HTMLInputElement::postDispatchEventHandler(
    Event* event,
    EventDispatchHandlingState* state) {
  if (!state)
    return;
  m_inputTypeView->didDispatchClick(event,
                                    *static_cast<ClickHandlingState*>(state));
}

void HTMLInputElement::defaultEventHandler(Event* evt) {
  if (evt->isMouseEvent() && evt->type() == EventTypeNames::click &&
      toMouseEvent(evt)->button() ==
          static_cast<short>(WebPointerProperties::Button::Left)) {
    m_inputTypeView->handleClickEvent(toMouseEvent(evt));
    if (evt->defaultHandled())
      return;
  }

  if (evt->isKeyboardEvent() && evt->type() == EventTypeNames::keydown) {
    m_inputTypeView->handleKeydownEvent(toKeyboardEvent(evt));
    if (evt->defaultHandled())
      return;
  }

  // Call the base event handler before any of our own event handling for almost
  // all events in text fields.  Makes editing keyboard handling take precedence
  // over the keydown and keypress handling in this function.
  bool callBaseClassEarly =
      isTextField() && (evt->type() == EventTypeNames::keydown ||
                        evt->type() == EventTypeNames::keypress);
  if (callBaseClassEarly) {
    TextControlElement::defaultEventHandler(evt);
    if (evt->defaultHandled())
      return;
  }

  // DOMActivate events cause the input to be "activated" - in the case of image
  // and submit inputs, this means actually submitting the form. For reset
  // inputs, the form is reset. These events are sent when the user clicks on
  // the element, or presses enter while it is the active element. JavaScript
  // code wishing to activate the element must dispatch a DOMActivate event - a
  // click event will not do the job.
  if (evt->type() == EventTypeNames::DOMActivate) {
    m_inputTypeView->handleDOMActivateEvent(evt);
    if (evt->defaultHandled())
      return;
  }

  // Use key press event here since sending simulated mouse events
  // on key down blocks the proper sending of the key press event.
  if (evt->isKeyboardEvent() && evt->type() == EventTypeNames::keypress) {
    m_inputTypeView->handleKeypressEvent(toKeyboardEvent(evt));
    if (evt->defaultHandled())
      return;
  }

  if (evt->isKeyboardEvent() && evt->type() == EventTypeNames::keyup) {
    m_inputTypeView->handleKeyupEvent(toKeyboardEvent(evt));
    if (evt->defaultHandled())
      return;
  }

  if (m_inputTypeView->shouldSubmitImplicitly(evt)) {
    // FIXME: Remove type check.
    if (type() == InputTypeNames::search)
      document().postTask(BLINK_FROM_HERE,
                          createSameThreadTask(&HTMLInputElement::onSearch,
                                               wrapPersistent(this)));
    // Form submission finishes editing, just as loss of focus does.
    // If there was a change, send the event now.
    if (wasChangedSinceLastFormControlChangeEvent())
      dispatchFormControlChangeEvent();

    HTMLFormElement* formForSubmission = m_inputTypeView->formForSubmission();
    // Form may never have been present, or may have been destroyed by code
    // responding to the change event.
    if (formForSubmission)
      formForSubmission->submitImplicitly(evt, canTriggerImplicitSubmission());

    evt->setDefaultHandled();
    return;
  }

  if (evt->isBeforeTextInsertedEvent())
    m_inputTypeView->handleBeforeTextInsertedEvent(
        static_cast<BeforeTextInsertedEvent*>(evt));

  if (evt->isMouseEvent() && evt->type() == EventTypeNames::mousedown) {
    m_inputTypeView->handleMouseDownEvent(toMouseEvent(evt));
    if (evt->defaultHandled())
      return;
  }

  m_inputTypeView->forwardEvent(evt);

  if (!callBaseClassEarly && !evt->defaultHandled())
    TextControlElement::defaultEventHandler(evt);
}

bool HTMLInputElement::willRespondToMouseClickEvents() {
  // FIXME: Consider implementing willRespondToMouseClickEvents() in InputType
  // if more accurate results are necessary.
  if (!isDisabledFormControl())
    return true;

  return TextControlElement::willRespondToMouseClickEvents();
}

bool HTMLInputElement::isURLAttribute(const Attribute& attribute) const {
  return attribute.name() == srcAttr || attribute.name() == formactionAttr ||
         TextControlElement::isURLAttribute(attribute);
}

bool HTMLInputElement::hasLegalLinkAttribute(const QualifiedName& name) const {
  return m_inputType->hasLegalLinkAttribute(name) ||
         TextControlElement::hasLegalLinkAttribute(name);
}

const QualifiedName& HTMLInputElement::subResourceAttributeName() const {
  return m_inputType->subResourceAttributeName();
}

const AtomicString& HTMLInputElement::defaultValue() const {
  return fastGetAttribute(valueAttr);
}

static inline bool isRFC2616TokenCharacter(UChar ch) {
  return isASCII(ch) && ch > ' ' && ch != '"' && ch != '(' && ch != ')' &&
         ch != ',' && ch != '/' && (ch < ':' || ch > '@') &&
         (ch < '[' || ch > ']') && ch != '{' && ch != '}' && ch != 0x7f;
}

static bool isValidMIMEType(const String& type) {
  size_t slashPosition = type.find('/');
  if (slashPosition == kNotFound || !slashPosition ||
      slashPosition == type.length() - 1)
    return false;
  for (size_t i = 0; i < type.length(); ++i) {
    if (!isRFC2616TokenCharacter(type[i]) && i != slashPosition)
      return false;
  }
  return true;
}

static bool isValidFileExtension(const String& type) {
  if (type.length() < 2)
    return false;
  return type[0] == '.';
}

static Vector<String> parseAcceptAttribute(const String& acceptString,
                                           bool (*predicate)(const String&)) {
  Vector<String> types;
  if (acceptString.isEmpty())
    return types;

  Vector<String> splitTypes;
  acceptString.split(',', false, splitTypes);
  for (const String& splitType : splitTypes) {
    String trimmedType = stripLeadingAndTrailingHTMLSpaces(splitType);
    if (trimmedType.isEmpty())
      continue;
    if (!predicate(trimmedType))
      continue;
    types.append(trimmedType.lower());
  }

  return types;
}

Vector<String> HTMLInputElement::acceptMIMETypes() {
  return parseAcceptAttribute(fastGetAttribute(acceptAttr), isValidMIMEType);
}

Vector<String> HTMLInputElement::acceptFileExtensions() {
  return parseAcceptAttribute(fastGetAttribute(acceptAttr),
                              isValidFileExtension);
}

const AtomicString& HTMLInputElement::alt() const {
  return fastGetAttribute(altAttr);
}

bool HTMLInputElement::multiple() const {
  return fastHasAttribute(multipleAttr);
}

void HTMLInputElement::setSize(unsigned size) {
  setUnsignedIntegralAttribute(sizeAttr, size);
}

void HTMLInputElement::setSize(unsigned size, ExceptionState& exceptionState) {
  if (!size)
    exceptionState.throwDOMException(
        IndexSizeError, "The value provided is 0, which is an invalid size.");
  else
    setSize(size);
}

KURL HTMLInputElement::src() const {
  return document().completeURL(fastGetAttribute(srcAttr));
}

FileList* HTMLInputElement::files() const {
  return m_inputType->files();
}

void HTMLInputElement::setFiles(FileList* files) {
  m_inputType->setFiles(files);
}

bool HTMLInputElement::receiveDroppedFiles(const DragData* dragData) {
  return m_inputType->receiveDroppedFiles(dragData);
}

String HTMLInputElement::droppedFileSystemId() {
  return m_inputType->droppedFileSystemId();
}

bool HTMLInputElement::canReceiveDroppedFiles() const {
  return m_canReceiveDroppedFiles;
}

void HTMLInputElement::setCanReceiveDroppedFiles(bool canReceiveDroppedFiles) {
  if (!!m_canReceiveDroppedFiles == canReceiveDroppedFiles)
    return;
  m_canReceiveDroppedFiles = canReceiveDroppedFiles;
  if (layoutObject())
    layoutObject()->updateFromElement();
}

String HTMLInputElement::sanitizeValue(const String& proposedValue) const {
  return m_inputType->sanitizeValue(proposedValue);
}

String HTMLInputElement::localizeValue(const String& proposedValue) const {
  if (proposedValue.isNull())
    return proposedValue;
  return m_inputType->localizeValue(proposedValue);
}

bool HTMLInputElement::isInRange() const {
  return willValidate() && m_inputType->isInRange(value());
}

bool HTMLInputElement::isOutOfRange() const {
  return willValidate() && m_inputType->isOutOfRange(value());
}

bool HTMLInputElement::isRequiredFormControl() const {
  return m_inputType->supportsRequired() && isRequired();
}

bool HTMLInputElement::matchesReadOnlyPseudoClass() const {
  return m_inputType->supportsReadOnly() && isReadOnly();
}

bool HTMLInputElement::matchesReadWritePseudoClass() const {
  return m_inputType->supportsReadOnly() && !isReadOnly();
}

void HTMLInputElement::onSearch() {
  m_inputType->dispatchSearchEvent();
}

void HTMLInputElement::updateClearButtonVisibility() {
  m_inputTypeView->updateClearButtonVisibility();
}

void HTMLInputElement::willChangeForm() {
  removeFromRadioButtonGroup();
  TextControlElement::willChangeForm();
}

void HTMLInputElement::didChangeForm() {
  TextControlElement::didChangeForm();
  addToRadioButtonGroup();
}

Node::InsertionNotificationRequest HTMLInputElement::insertedInto(
    ContainerNode* insertionPoint) {
  TextControlElement::insertedInto(insertionPoint);
  if (insertionPoint->isConnected() && !form())
    addToRadioButtonGroup();
  resetListAttributeTargetObserver();
  logAddElementIfIsolatedWorldAndInDocument("input", typeAttr, formactionAttr);
  return InsertionShouldCallDidNotifySubtreeInsertions;
}

void HTMLInputElement::removedFrom(ContainerNode* insertionPoint) {
  m_inputTypeView->closePopupView();
  if (insertionPoint->isConnected() && !form())
    removeFromRadioButtonGroup();
  TextControlElement::removedFrom(insertionPoint);
  DCHECK(!isConnected());
  resetListAttributeTargetObserver();
}

void HTMLInputElement::didMoveToNewDocument(Document& oldDocument) {
  if (imageLoader())
    imageLoader()->elementDidMoveToNewDocument();

  // FIXME: Remove type check.
  if (type() == InputTypeNames::radio)
    treeScope().radioButtonGroupScope().removeButton(this);

  TextControlElement::didMoveToNewDocument(oldDocument);
}

bool HTMLInputElement::recalcWillValidate() const {
  return m_inputType->supportsValidation() &&
         TextControlElement::recalcWillValidate();
}

void HTMLInputElement::requiredAttributeChanged() {
  TextControlElement::requiredAttributeChanged();
  if (RadioButtonGroupScope* scope = radioButtonGroupScope())
    scope->requiredAttributeChanged(this);
  m_inputTypeView->requiredAttributeChanged();
}

void HTMLInputElement::disabledAttributeChanged() {
  TextControlElement::disabledAttributeChanged();
  m_inputTypeView->disabledAttributeChanged();
}

void HTMLInputElement::selectColorInColorChooser(const Color& color) {
  if (ColorChooserClient* client = m_inputType->colorChooserClient())
    client->didChooseColor(color);
}

void HTMLInputElement::endColorChooser() {
  if (ColorChooserClient* client = m_inputType->colorChooserClient())
    client->didEndChooser();
}

HTMLElement* HTMLInputElement::list() const {
  return dataList();
}

HTMLDataListElement* HTMLInputElement::dataList() const {
  if (!m_hasNonEmptyList)
    return nullptr;

  if (!m_inputType->shouldRespectListAttribute())
    return nullptr;

  Element* element = treeScope().getElementById(fastGetAttribute(listAttr));
  if (!element)
    return nullptr;
  if (!isHTMLDataListElement(*element))
    return nullptr;

  return toHTMLDataListElement(element);
}

bool HTMLInputElement::hasValidDataListOptions() const {
  HTMLDataListElement* dataList = this->dataList();
  if (!dataList)
    return false;
  HTMLDataListOptionsCollection* options = dataList->options();
  for (unsigned i = 0; HTMLOptionElement* option = options->item(i); ++i) {
    if (isValidValue(option->value()))
      return true;
  }
  return false;
}

HeapVector<Member<HTMLOptionElement>>
HTMLInputElement::filteredDataListOptions() const {
  HeapVector<Member<HTMLOptionElement>> filtered;
  HTMLDataListElement* dataList = this->dataList();
  if (!dataList)
    return filtered;

  String value = innerEditorValue();
  if (multiple() && type() == InputTypeNames::email) {
    Vector<String> emails;
    value.split(',', true, emails);
    if (!emails.isEmpty())
      value = emails.last().stripWhiteSpace();
  }

  HTMLDataListOptionsCollection* options = dataList->options();
  filtered.reserveCapacity(options->length());
  value = value.foldCase();
  for (unsigned i = 0; i < options->length(); ++i) {
    HTMLOptionElement* option = options->item(i);
    DCHECK(option);
    if (!value.isEmpty()) {
      // Firefox shows OPTIONs with matched labels, Edge shows OPTIONs
      // with matches values. We show both.
      if (option->value().foldCase().find(value) == kNotFound &&
          option->label().foldCase().find(value) == kNotFound)
        continue;
    }
    // TODO(tkent): Should allow invalid strings. crbug.com/607097.
    if (!isValidValue(option->value()))
      continue;
    filtered.append(option);
  }
  return filtered;
}

void HTMLInputElement::setListAttributeTargetObserver(
    ListAttributeTargetObserver* newObserver) {
  if (m_listAttributeTargetObserver)
    m_listAttributeTargetObserver->unregister();
  m_listAttributeTargetObserver = newObserver;
}

void HTMLInputElement::resetListAttributeTargetObserver() {
  if (isConnected())
    setListAttributeTargetObserver(
        ListAttributeTargetObserver::create(fastGetAttribute(listAttr), this));
  else
    setListAttributeTargetObserver(nullptr);
}

void HTMLInputElement::listAttributeTargetChanged() {
  m_inputTypeView->listAttributeTargetChanged();
}

bool HTMLInputElement::isSteppable() const {
  return m_inputType->isSteppable();
}

bool HTMLInputElement::isTextButton() const {
  return m_inputType->isTextButton();
}

bool HTMLInputElement::isEnumeratable() const {
  return m_inputType->isEnumeratable();
}

bool HTMLInputElement::supportLabels() const {
  return m_inputType->isInteractiveContent();
}

bool HTMLInputElement::matchesDefaultPseudoClass() const {
  return m_inputType->matchesDefaultPseudoClass();
}

bool HTMLInputElement::shouldAppearChecked() const {
  return checked() && m_inputType->isCheckable();
}

void HTMLInputElement::setPlaceholderVisibility(bool visible) {
  m_isPlaceholderVisible = visible;
}

bool HTMLInputElement::supportsPlaceholder() const {
  return m_inputType->supportsPlaceholder();
}

void HTMLInputElement::updatePlaceholderText() {
  return m_inputTypeView->updatePlaceholderText();
}

bool HTMLInputElement::supportsAutocapitalize() const {
  return m_inputType->supportsAutocapitalize();
}

const AtomicString& HTMLInputElement::defaultAutocapitalize() const {
  return m_inputType->defaultAutocapitalize();
}

String HTMLInputElement::defaultToolTip() const {
  return m_inputType->defaultToolTip(*m_inputTypeView);
}

bool HTMLInputElement::shouldAppearIndeterminate() const {
  return m_inputType->shouldAppearIndeterminate();
}

bool HTMLInputElement::isInRequiredRadioButtonGroup() {
  // TODO(tkent): Remove type check.
  DCHECK_EQ(type(), InputTypeNames::radio);
  if (RadioButtonGroupScope* scope = radioButtonGroupScope())
    return scope->isInRequiredGroup(this);
  return false;
}

const AtomicString& HTMLInputElement::nwworkingdir() const
{
  return fastGetAttribute(nwworkingdirAttr);
}

void HTMLInputElement::setNwworkingdir(const AtomicString& value)
{
  setAttribute(nwworkingdirAttr, value);
}

HTMLInputElement* HTMLInputElement::checkedRadioButtonForGroup() {
  if (checked())
    return this;
  if (RadioButtonGroupScope* scope = radioButtonGroupScope())
    return scope->checkedButtonForGroup(name());
  return nullptr;
}

String HTMLInputElement::nwsaveas() const
{
  return fastGetAttribute(nwsaveasAttr);
}

void HTMLInputElement::setNwsaveas(const String& value)
{
  setAttribute(nwsaveasAttr, AtomicString(value));
}

RadioButtonGroupScope* HTMLInputElement::radioButtonGroupScope() const {
  // FIXME: Remove type check.
  if (type() != InputTypeNames::radio)
    return nullptr;
  if (HTMLFormElement* formElement = form())
    return &formElement->radioButtonGroupScope();
  if (isConnected())
    return &treeScope().radioButtonGroupScope();
  return nullptr;
}

unsigned HTMLInputElement::sizeOfRadioGroup() const {
  RadioButtonGroupScope* scope = radioButtonGroupScope();
  if (!scope)
    return 0;
  return scope->groupSizeFor(this);
}

inline void HTMLInputElement::addToRadioButtonGroup() {
  if (RadioButtonGroupScope* scope = radioButtonGroupScope())
    scope->addButton(this);
}

inline void HTMLInputElement::removeFromRadioButtonGroup() {
  if (RadioButtonGroupScope* scope = radioButtonGroupScope())
    scope->removeButton(this);
}

unsigned HTMLInputElement::height() const {
  return m_inputType->height();
}

unsigned HTMLInputElement::width() const {
  return m_inputType->width();
}

void HTMLInputElement::setHeight(unsigned height) {
  setUnsignedIntegralAttribute(heightAttr, height);
}

void HTMLInputElement::setWidth(unsigned width) {
  setUnsignedIntegralAttribute(widthAttr, width);
}

ListAttributeTargetObserver* ListAttributeTargetObserver::create(
    const AtomicString& id,
    HTMLInputElement* element) {
  return new ListAttributeTargetObserver(id, element);
}

ListAttributeTargetObserver::ListAttributeTargetObserver(
    const AtomicString& id,
    HTMLInputElement* element)
    : IdTargetObserver(element->treeScope().idTargetObserverRegistry(), id),
      m_element(element) {}

DEFINE_TRACE(ListAttributeTargetObserver) {
  visitor->trace(m_element);
  IdTargetObserver::trace(visitor);
}

void ListAttributeTargetObserver::idTargetChanged() {
  m_element->listAttributeTargetChanged();
}

void HTMLInputElement::setRangeText(const String& replacement,
                                    ExceptionState& exceptionState) {
  if (!m_inputType->supportsSelectionAPI()) {
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return;
  }

  TextControlElement::setRangeText(replacement, exceptionState);
}

void HTMLInputElement::setRangeText(const String& replacement,
                                    unsigned start,
                                    unsigned end,
                                    const String& selectionMode,
                                    ExceptionState& exceptionState) {
  if (!m_inputType->supportsSelectionAPI()) {
    exceptionState.throwDOMException(InvalidStateError,
                                     "The input element's type ('" +
                                         m_inputType->formControlType() +
                                         "') does not support selection.");
    return;
  }

  TextControlElement::setRangeText(replacement, start, end, selectionMode,
                                   exceptionState);
}

bool HTMLInputElement::setupDateTimeChooserParameters(
    DateTimeChooserParameters& parameters) {
  if (!document().view())
    return false;

  parameters.type = type();
  parameters.minimum = minimum();
  parameters.maximum = maximum();
  parameters.required = isRequired();
  if (!RuntimeEnabledFeatures::langAttributeAwareFormControlUIEnabled()) {
    parameters.locale = defaultLanguage();
  } else {
    AtomicString computedLocale = computeInheritedLanguage();
    parameters.locale =
        computedLocale.isEmpty() ? defaultLanguage() : computedLocale;
  }

  StepRange stepRange = createStepRange(RejectAny);
  if (stepRange.hasStep()) {
    parameters.step = stepRange.step().toDouble();
    parameters.stepBase = stepRange.stepBase().toDouble();
  } else {
    parameters.step = 1.0;
    parameters.stepBase = 0;
  }

  parameters.anchorRectInScreen =
      document().view()->contentsToScreen(pixelSnappedBoundingBox());
  parameters.currentValue = value();
  parameters.doubleValue = m_inputType->valueAsDouble();
  parameters.isAnchorElementRTL =
      m_inputTypeView->computedTextDirection() == RTL;
  if (HTMLDataListElement* dataList = this->dataList()) {
    HTMLDataListOptionsCollection* options = dataList->options();
    for (unsigned i = 0; HTMLOptionElement* option = options->item(i); ++i) {
      if (!isValidValue(option->value()))
        continue;
      DateTimeSuggestion suggestion;
      suggestion.value =
          m_inputType->parseToNumber(option->value(), Decimal::nan())
              .toDouble();
      if (std::isnan(suggestion.value))
        continue;
      suggestion.localizedValue = localizeValue(option->value());
      suggestion.label =
          option->value() == option->label() ? String() : option->label();
      parameters.suggestions.append(suggestion);
    }
  }
  return true;
}

bool HTMLInputElement::supportsInputModeAttribute() const {
  return m_inputType->supportsInputModeAttribute();
}

void HTMLInputElement::setShouldRevealPassword(bool value) {
  if (!!m_shouldRevealPassword == value)
    return;
  m_shouldRevealPassword = value;
  lazyReattachIfAttached();
}

bool HTMLInputElement::isInteractiveContent() const {
  return m_inputType->isInteractiveContent();
}

bool HTMLInputElement::supportsAutofocus() const {
  return m_inputType->isInteractiveContent();
}

PassRefPtr<ComputedStyle> HTMLInputElement::customStyleForLayoutObject() {
  return m_inputTypeView->customStyleForLayoutObject(
      originalStyleForLayoutObject());
}

bool HTMLInputElement::shouldDispatchFormControlChangeEvent(String& oldValue,
                                                            String& newValue) {
  return m_inputType->shouldDispatchFormControlChangeEvent(oldValue, newValue);
}

void HTMLInputElement::didNotifySubtreeInsertionsToDocument() {
  listAttributeTargetChanged();
}

AXObject* HTMLInputElement::popupRootAXObject() {
  return m_inputTypeView->popupRootAXObject();
}

void HTMLInputElement::ensureFallbackContent() {
  m_inputTypeView->ensureFallbackContent();
}

void HTMLInputElement::ensurePrimaryContent() {
  m_inputTypeView->ensurePrimaryContent();
}

bool HTMLInputElement::hasFallbackContent() const {
  return m_inputTypeView->hasFallbackContent();
}

void HTMLInputElement::setFilesFromPaths(const Vector<String>& paths) {
  return m_inputType->setFilesFromPaths(paths);
}

}  // namespace blink
