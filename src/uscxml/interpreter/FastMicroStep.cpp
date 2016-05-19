/**
 *  @file
 *  @author     2012-2016 Stefan Radomski (stefan.radomski@cs.tu-darmstadt.de)
 *  @copyright  Simplified BSD
 *
 *  @cond
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the FreeBSD license as published by the FreeBSD
 *  project.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *  You should have received a copy of the FreeBSD license along with this
 *  program. If not, see <http://www.opensource.org/licenses/bsd-license>.
 *  @endcond
 */

#undef USCXML_VERBOSE

#include "FastMicroStep.h"
#include "uscxml/util/DOM.h"
#include "uscxml/util/String.h"
#include "uscxml/util/Predicates.h"
#include "uscxml/util/Convenience.h"
#include "uscxml/interpreter/InterpreterMonitor.h"

#include <easylogging++.h>

#define BIT_ANY_SET(b) (!b.none())
#define BIT_HAS(idx, bitset) (bitset[idx])
#define BIT_HAS_AND(bitset1, bitset2) bitset1.intersects(bitset2)
#define BIT_SET_AT(idx, bitset) bitset[idx] = true;
#define BIT_CLEAR(idx, bitset) bitset[idx] = false;

#define USCXML_GET_TRANS(i) (*_transitions[i])
#define USCXML_GET_STATE(i) (*_states[i])

#define USCXML_CTX_PRISTINE           0x00
#define USCXML_CTX_SPONTANEOUS        0x01
#define USCXML_CTX_INITIALIZED        0x02
#define USCXML_CTX_TOP_LEVEL_FINAL    0x04
#define USCXML_CTX_TRANSITION_FOUND   0x08
#define USCXML_CTX_FINISHED           0x10
#define USCXML_CTX_STABLE             0x20 // only needed to signal onStable once

#define USCXML_TRANS_SPONTANEOUS      0x01
#define USCXML_TRANS_TARGETLESS       0x02
#define USCXML_TRANS_INTERNAL         0x04
#define USCXML_TRANS_HISTORY          0x08
#define USCXML_TRANS_INITIAL          0x10

#define USCXML_STATE_ATOMIC           0x01
#define USCXML_STATE_PARALLEL         0x02
#define USCXML_STATE_COMPOUND         0x03
#define USCXML_STATE_FINAL            0x04
#define USCXML_STATE_HISTORY_DEEP     0x05
#define USCXML_STATE_HISTORY_SHALLOW  0x06
#define USCXML_STATE_INITIAL          0x07
#define USCXML_STATE_HAS_HISTORY      0x80  /* highest bit */
#define USCXML_STATE_MASK(t)     (t & 0x7F) /* mask highest bit */

#define USCXML_NUMBER_STATES _states.size()
#define USCXML_NUMBER_TRANS _transitions.size()

#ifdef __GNUC__
#  define likely(x)       (__builtin_expect(!!(x), 1))
#  define unlikely(x)     (__builtin_expect(!!(x), 0))
#else
#  define likely(x)       (x)
#  define unlikely(x)     (x)
#endif

namespace uscxml {

using namespace XERCESC_NS;

FastMicroStep::FastMicroStep(MicroStepCallbacks* callbacks)
	: MicroStepImpl(callbacks), _flags(USCXML_CTX_PRISTINE), _isInitialized(false), _isCancelled(false) {
}

FastMicroStep::~FastMicroStep() {
	for (size_t i = 0; i < _states.size(); i++) {
		delete(_states[i]);
	}
	for (size_t i = 0; i < _transitions.size(); i++) {
		delete(_transitions[i]);
	}
}

void FastMicroStep::resortStates(DOMNode* node, const X& xmlPrefix) {
	if (node->getNodeType() != DOMNode::ELEMENT_NODE)
		return;

	/**
	 initials
	 deep histories
	 shallow histories
	 everything else
	 */

	DOMElement* element = dynamic_cast<DOMElement*>(node);

	// shallow history states to top
	DOMNode* child = element->getFirstChild();
	while(child) {
		resortStates(child, xmlPrefix);
		if (child->getNodeType() == DOMNode::ELEMENT_NODE &&
		        TAGNAME_CAST(child) == xmlPrefix.str() + "history" &&
		        (!HAS_ATTR(element, "type") || iequals(ATTR(element, "type"), "shallow"))) {

			DOMNode* tmp = child->getNextSibling();
			if (child != element->getFirstChild()) {
				element->insertBefore(child, element->getFirstChild());
			}
			child = tmp;
		} else {
			child = child->getNextSibling();
		}
	}

	// deep history states to top
	child = element->getFirstChild();
	while(child) {
		resortStates(child, xmlPrefix);
		if (child->getNodeType() == DOMNode::ELEMENT_NODE &&
		        TAGNAME_CAST(child) == xmlPrefix.str() + "history" &&
		        HAS_ATTR(element, "type") &&
		        iequals(ATTR(element, "type"), "deep")) {

			DOMNode* tmp = child->getNextSibling();
			if (child != element->getFirstChild()) {
				element->insertBefore(child, element->getFirstChild());
			}
			child = tmp;
		} else {
			child = child->getNextSibling();
		}
	}

	// initial states on top of histories even
	child = element->getFirstChild();
	while(child) {
		resortStates(child, xmlPrefix);
		if (child->getNodeType() == DOMNode::ELEMENT_NODE && LOCALNAME_CAST(child) == "initial") {
			DOMNode* tmp = child->getNextSibling();
			if (child != element->getFirstChild()) {
				element->insertBefore(child, element->getFirstChild());
			}
			child = tmp;
		} else {
			child = child->getNextSibling();
		}
	}
}

void FastMicroStep::init(XERCESC_NS::DOMElement* scxml) {

	_scxml = scxml;
	_binding = (HAS_ATTR(_scxml, "binding") && iequals(ATTR(_scxml, "binding"), "late") ? LATE : EARLY);
	_xmlPrefix = _scxml->getPrefix();
	_xmlNS = _scxml->getNamespaceURI();
	if (_xmlPrefix) {
		_xmlPrefix = std::string(_xmlPrefix) + ":";
	}

	resortStates(_scxml, _xmlPrefix);

	// TODO: https://github.com/tklab-tud/uscxml/blob/master/src/uscxml/transform/ChartToC.cpp#L244


	/** -- All things states -- */

	std::list<XERCESC_NS::DOMElement*> tmp;
	size_t i, j;

	tmp = DOMUtils::inDocumentOrder({
		_xmlPrefix.str() + "state",
		_xmlPrefix.str() + "parallel",
		_xmlPrefix.str() + "scxml",
		_xmlPrefix.str() + "initial",
		_xmlPrefix.str() + "final",
		_xmlPrefix.str() + "history"
	}, _scxml);

	_states.resize(tmp.size());
	_configuration.resize(tmp.size());
	_history.resize(tmp.size());
	_initializedData.resize(tmp.size());
	_invocations.resize(tmp.size());

	for (i = 0; i < _states.size(); i++) {
		_states[i] = new State();
		_states[i]->documentOrder = i;
		_states[i]->element = tmp.front();
		_states[i]->element->setUserData(X("uscxmlState"), _states[i], NULL);
		_states[i]->completion.resize(_states.size());
		_states[i]->ancestors.resize(_states.size());
		_states[i]->children.resize(_states.size());
		tmp.pop_front();
	}
	assert(tmp.size() == 0);

	if (_binding == Binding::EARLY && _states.size() > 0) {
		// add all data elements to the first state
		std::list<DOMElement*> dataModels = DOMUtils::filterChildElements(_xmlPrefix.str() + "datamodel", _states[0]->element, true);
		dataModels.erase(std::remove_if(dataModels.begin(),
		                                dataModels.end(),
		[](DOMElement* elem) {
			return isInEmbeddedDocument(elem);
		}),
		dataModels.end());

		_states[0]->data = DOMUtils::filterChildElements(_xmlPrefix.str() + "data", dataModels, false);
	}

	for (i = 0; i < _states.size(); i++) {
		// collect states with an id attribute
		if (HAS_ATTR(_states[i]->element, "id")) {
			_stateIds[ATTR(_states[i]->element, "id")] = i;
		}

		// check for executable content and datamodels
		if (_states[i]->element->getChildElementCount() > 0) {
			_states[i]->onEntry = DOMUtils::filterChildElements(_xmlPrefix.str() + "onentry", _states[i]->element);
			_states[i]->onExit = DOMUtils::filterChildElements(_xmlPrefix.str() + "onexit", _states[i]->element);
			_states[i]->invoke = DOMUtils::filterChildElements(_xmlPrefix.str() + "invoke", _states[i]->element);

			if (i == 0) {
				// have global scripts as onentry of <scxml>
				_states[i]->onEntry = DOMUtils::filterChildElements(_xmlPrefix.str() + "script", _states[i]->element, false);
			}

			std::list<DOMElement*> doneDatas = DOMUtils::filterChildElements(_xmlPrefix.str() + "donedata", _states[i]->element);
			if (doneDatas.size() > 0) {
				_states[i]->doneData = doneDatas.front();
			}

			if (_binding == Binding::LATE) {
				std::list<DOMElement*> dataModels = DOMUtils::filterChildElements(_xmlPrefix.str() + "datamodel", _states[i]->element);
				if (dataModels.size() > 0) {
					_states[i]->data = DOMUtils::filterChildElements(_xmlPrefix.str() + "data", dataModels, false);
				}
			}
		}


		// set the states type
		if (false) {
		} else if (iequals(TAGNAME(_states[i]->element), _xmlPrefix.str() + "initial")) {
			_states[i]->type = USCXML_STATE_INITIAL;
		} else if (isFinal(_states[i]->element)) {
			_states[i]->type =  USCXML_STATE_FINAL;
		} else if (isHistory(_states[i]->element)) {
			if (HAS_ATTR(_states[i]->element, "type") && iequals(ATTR(_states[i]->element, "type"), "deep")) {
				_states[i]->type = USCXML_STATE_HISTORY_DEEP;
			} else {
				_states[i]->type = USCXML_STATE_HISTORY_SHALLOW;
			}
		} else if (isAtomic(_states[i]->element)) {
			_states[i]->type = USCXML_STATE_ATOMIC;
		} else if (isParallel(_states[i]->element)) {
			_states[i]->type = USCXML_STATE_PARALLEL;
		} else if (isCompound(_states[i]->element)) {
			_states[i]->type = USCXML_STATE_COMPOUND;
		} else { // <scxml>
			_states[i]->type = USCXML_STATE_COMPOUND;
		}

		// establish the states' completion
		std::list<DOMElement*> completion = getCompletion(_states[i]->element);
		for (j = 0; j < _states.size(); j++) {
			if (!completion.empty() && _states[j]->element == completion.front()) {
				_states[i]->completion[j] = true;
				completion.pop_front();
			} else {
				_states[i]->completion[j] = false;
			}
		}
		assert(completion.size() == 0);

		// this is set when establishing the completion
		if (_states[i]->element->getUserData(X("hasHistoryChild")) == _states[i]) {
			_states[i]->type |= USCXML_STATE_HAS_HISTORY;
		}

		// establish the states' ancestors
		DOMNode* parent = _states[i]->element->getParentNode();
		if (parent && parent->getNodeType() == DOMNode::ELEMENT_NODE) {
			State* uscxmlState = (State*)parent->getUserData(X("uscxmlState"));
			// parent
			_states[i]->parent = uscxmlState->documentOrder;
		}

		while(parent && parent->getNodeType() == DOMNode::ELEMENT_NODE) {
			State* uscxmlState = (State*)parent->getUserData(X("uscxmlState"));

			// ancestors
			BIT_SET_AT(uscxmlState->documentOrder, _states[i]->ancestors);

			// children
			BIT_SET_AT(i, uscxmlState->children);
			parent = parent->getParentNode();
		}
	}


	/** -- All things transitions -- */

	tmp = DOMUtils::inPostFixOrder({_xmlPrefix.str() + "transition"}, _scxml);
	_transitions.resize(tmp.size());

	for (i = 0; i < _transitions.size(); i++) {
		_transitions[i] = new Transition();
		_transitions[i]->element = tmp.front();
		_transitions[i]->conflicts.resize(_transitions.size());
		_transitions[i]->exitSet.resize(_states.size());
		_transitions[i]->target.resize(_states.size());
		tmp.pop_front();
	}
	assert(tmp.size() == 0);

	for (i = 0; i < _transitions.size(); i++) {

		// establish the transitions' exit set
		assert(_transitions[i]->element != NULL);
//		std::cout << "i: " << i << std::endl << std::flush;
		std::list<DOMElement*> exitList = getExitSet(_transitions[i]->element, _scxml);
		for (j = 0; j < _states.size(); j++) {
			if (!exitList.empty() && _states[j]->element == exitList.front()) {
				_transitions[i]->exitSet[j] = true;
				exitList.pop_front();
			} else {
				_transitions[i]->exitSet[j] = false;
			}
		}
		assert(exitList.size() == 0);

		// establish the transitions' conflict set
		for (j = 0; j < _transitions.size(); j++) {
			if (conflicts(_transitions[i]->element, _transitions[j]->element, _scxml)) {
				_transitions[i]->conflicts[j] = true;
			} else {
				_transitions[i]->conflicts[j] = false;
			}
		}

		// establish the transitions' target set
		std::list<std::string> targets = tokenize(ATTR(_transitions[i]->element, "target"));
		for (auto tIter = targets.begin(); tIter != targets.end(); tIter++) {
			if (_stateIds.find(*tIter) != _stateIds.end()) {
				_transitions[i]->target[_stateIds[*tIter]] = true;
			}
		}

		// the transition's source
		State* uscxmlState = (State*)(_transitions[i]->element->getParentNode()->getUserData(X("uscxmlState")));
		_transitions[i]->source = uscxmlState->documentOrder;


		// the transition's type
		if (!HAS_ATTR(_transitions[i]->element, "target")) {
			_transitions[i]->type |= USCXML_TRANS_TARGETLESS;
		}

		if (HAS_ATTR(_transitions[i]->element, "type") && iequals(ATTR(_transitions[i]->element, "type"), "internal")) {
			_transitions[i]->type |= USCXML_TRANS_INTERNAL;
		}

		if (!HAS_ATTR(_transitions[i]->element, "event")) {
			_transitions[i]->type |= USCXML_TRANS_SPONTANEOUS;
		}

		if (iequals(TAGNAME_CAST(_transitions[i]->element->getParentNode()), _xmlPrefix.str() + "history")) {
			_transitions[i]->type |= USCXML_TRANS_HISTORY;
		}

		if (iequals(TAGNAME_CAST(_transitions[i]->element->getParentNode()), _xmlPrefix.str() + "initial")) {
			_transitions[i]->type |= USCXML_TRANS_INITIAL;
		}


		// the transitions event and condition
		_transitions[i]->event = (HAS_ATTR(_transitions[i]->element, "event") ? ATTR(_transitions[i]->element, "event") : "");
		_transitions[i]->cond = (HAS_ATTR(_transitions[i]->element, "cond") ? ATTR(_transitions[i]->element, "cond") : "");

		// is there executable content?
		if (_transitions[i]->element->getChildElementCount() > 0) {
			_transitions[i]->onTrans = _transitions[i]->element;
		}

	}
	_isInitialized = true;
}

void FastMicroStep::markAsCancelled() {
	_isCancelled = true;
}

InterpreterState FastMicroStep::step(bool blocking) {
	if (!_isInitialized) {
		init(_scxml);
		return USCXML_INITIALIZED;
	}

	size_t i, j, k;

	boost::dynamic_bitset<> exitSet(_states.size(), false);
	boost::dynamic_bitset<> entrySet(_states.size(), false);
	boost::dynamic_bitset<> targetSet(_states.size(), false);
	boost::dynamic_bitset<> tmpStates(_states.size(), false);

	boost::dynamic_bitset<> conflicts(_transitions.size(), false);
	boost::dynamic_bitset<> transSet(_transitions.size(), false);

#ifdef USCXML_VERBOSE
	std::cerr << "Config: ";
	printStateNames(_configuration);
#endif

	if (_flags & USCXML_CTX_FINISHED)
		return USCXML_FINISHED;

	if (_flags & USCXML_CTX_TOP_LEVEL_FINAL) {
		USCXML_MONITOR_CALLBACK(_callbacks->getMonitor(), beforeCompletion);

		/* exit all remaining states */
		i = USCXML_NUMBER_STATES;
		while(i-- > 0) {
			if (BIT_HAS(i, _configuration)) {
				/* call all on exit handlers */
				for (auto exitIter = USCXML_GET_STATE(i).onExit.begin(); exitIter != USCXML_GET_STATE(i).onExit.end(); exitIter++) {
					try {
						_callbacks->process(*exitIter);
					} catch (...) {
						// do nothing and continue with next block
					}
				}
				/* Leave configuration intact */
//                BIT_CLEAR(i, _configuration);
			}

			if (BIT_HAS(i, _invocations)) {
				/* cancel all invokers */
				if (USCXML_GET_STATE(i).invoke.size() > 0) {
					for (auto invIter = USCXML_GET_STATE(i).invoke.begin(); invIter != USCXML_GET_STATE(i).invoke.end(); invIter++) {
						_callbacks->uninvoke(*invIter);
					}
				}
				BIT_CLEAR(i, _invocations);
			}
		}

		_flags |= USCXML_CTX_FINISHED;

		USCXML_MONITOR_CALLBACK(_callbacks->getMonitor(), afterCompletion);

		return USCXML_FINISHED;
	}


	if (_flags == USCXML_CTX_PRISTINE) {

		targetSet |= USCXML_GET_STATE(0).completion;
		_flags |= USCXML_CTX_SPONTANEOUS | USCXML_CTX_INITIALIZED;
		USCXML_MONITOR_CALLBACK(_callbacks->getMonitor(), beforeMicroStep);

		goto ESTABLISH_ENTRYSET;
	}

	if (_flags & USCXML_CTX_SPONTANEOUS) {
		_event = Event();
		goto SELECT_TRANSITIONS;
	}


	if ((_event = _callbacks->dequeueInternal())) {
		USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), beforeProcessingEvent, _event);
		goto SELECT_TRANSITIONS;
	}

	/* manage invocations */
	for (i = 0; i < USCXML_NUMBER_STATES; i++) {
		/* uninvoke */
		if (!BIT_HAS(i, _configuration) && BIT_HAS(i, _invocations)) {
			if (USCXML_GET_STATE(i).invoke.size() > 0) {
				for (auto invIter = USCXML_GET_STATE(i).invoke.begin(); invIter != USCXML_GET_STATE(i).invoke.end(); invIter++) {
					_callbacks->uninvoke(*invIter);
				}
			}
			BIT_CLEAR(i, _invocations)
		}
		/* invoke */
		if (BIT_HAS(i, _configuration) && !BIT_HAS(i, _invocations)) {
			if (USCXML_GET_STATE(i).invoke.size() > 0) {
				for (auto invIter = USCXML_GET_STATE(i).invoke.begin(); invIter != USCXML_GET_STATE(i).invoke.end(); invIter++) {
					try {
						_callbacks->invoke(*invIter);
					} catch (...) {
					}
				}
			}
			BIT_SET_AT(i, _invocations)
		}
	}

	// we dequeued all internal events and ought to signal stable configuration
	if (!(_flags & USCXML_CTX_STABLE)) {
		USCXML_MONITOR_CALLBACK(_callbacks->getMonitor(), onStableConfiguration);
		_microstepConfigurations.clear();
		_flags |= USCXML_CTX_STABLE;
	}

	if ((_event = _callbacks->dequeueExternal(blocking))) {
		USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), beforeProcessingEvent, _event);
		goto SELECT_TRANSITIONS;
	}

	if (_isCancelled) {
		// finalize and exit
		_flags |= USCXML_CTX_TOP_LEVEL_FINAL;
		return USCXML_CANCELLED;
	}

//	if (blocking) // we received the empty event to unblock
//        return USCXML_IDLE; // we return IDLE nevertheless

	return USCXML_IDLE;

SELECT_TRANSITIONS:

	// we read an event - unset stable to signal onstable again later
	_flags &= ~USCXML_CTX_STABLE;

	for (i = 0; i < _transitions.size(); i++) {
		/* never select history or initial transitions automatically */
		if unlikely(USCXML_GET_TRANS(i).type & (USCXML_TRANS_HISTORY | USCXML_TRANS_INITIAL))
			continue;

		/* is the transition active? */
		if (BIT_HAS(USCXML_GET_TRANS(i).source, _configuration)) {
			/* is it non-conflicting? */
			if (!BIT_HAS(i, conflicts)) {
				/* is it spontaneous with an event or vice versa? */
				if ((USCXML_GET_TRANS(i).event.size() == 0 && !_event) ||
				        (USCXML_GET_TRANS(i).event.size() != 0 && _event)) {
					/* is it enabled? */
					if ((!_event || _callbacks->isMatched(_event, USCXML_GET_TRANS(i).event)) &&
					        (USCXML_GET_TRANS(i).cond.size() == 0 || _callbacks->isTrue(USCXML_GET_TRANS(i).cond))) {

						/* remember that we found a transition */
						_flags |= USCXML_CTX_TRANSITION_FOUND;

						/* transitions that are pre-empted */
						conflicts |= USCXML_GET_TRANS(i).conflicts;

						/* states that are directly targeted (resolve as entry-set later) */
						targetSet |= USCXML_GET_TRANS(i).target;

						/* states that will be left */
						exitSet |= USCXML_GET_TRANS(i).exitSet;

						BIT_SET_AT(i, transSet);
					}
				}
			}
		}
	}

#ifdef USCXML_VERBOSE
	std::cerr << "Complete Exit: ";
	printStateNames(exitSet);
#endif

	exitSet &= _configuration;

	if (_flags & USCXML_CTX_TRANSITION_FOUND) {
		// trigger more sppontaneuous transitions
		_flags |= USCXML_CTX_SPONTANEOUS;
		_flags &= ~USCXML_CTX_TRANSITION_FOUND;
	} else {
		// spontaneuous transitions are exhausted
		_flags &= ~USCXML_CTX_SPONTANEOUS;
		return USCXML_MACROSTEPPED;
	}

	USCXML_MONITOR_CALLBACK(_callbacks->getMonitor(), beforeMicroStep);

#ifdef USCXML_VERBOSE
	std::cerr << "Targets: ";
	printStateNames(targetSet);
#endif

#ifdef USCXML_VERBOSE
	std::cerr << "Exiting: ";
	printStateNames(exitSet);
#endif

#ifdef USCXML_VERBOSE
	std::cerr << "History: ";
	printStateNames(_history);
#endif


	/* REMEMBER_HISTORY: */
	for (i = 0; i < _states.size(); i++) {
		if unlikely(USCXML_STATE_MASK(USCXML_GET_STATE(i).type) == USCXML_STATE_HISTORY_SHALLOW ||
		            USCXML_STATE_MASK(USCXML_GET_STATE(i).type) == USCXML_STATE_HISTORY_DEEP) {
			/* a history state whose parent is about to be exited */
			if unlikely(BIT_HAS(USCXML_GET_STATE(i).parent, exitSet)) {
				tmpStates = USCXML_GET_STATE(i).completion;

				/* set those states who were enabled */
				tmpStates &= _configuration;

				/* clear current history with completion mask */
				_history &= ~(USCXML_GET_STATE(i).completion);

				/* set history */
				_history |= tmpStates;

			}
		}
	}

ESTABLISH_ENTRYSET:
	/* calculate new entry set */
	entrySet = targetSet;

	/* iterate for ancestors */
	i = entrySet.find_first();
	while(i != boost::dynamic_bitset<>::npos) {
		entrySet |= USCXML_GET_STATE(i).ancestors;
		i = entrySet.find_next(i);
	}

	/* iterate for descendants */
	i = entrySet.find_first();
	while(i != boost::dynamic_bitset<>::npos) {


		switch (USCXML_STATE_MASK(USCXML_GET_STATE(i).type)) {
		case USCXML_STATE_FINAL:
		case USCXML_STATE_ATOMIC:
			break;

		case USCXML_STATE_PARALLEL: {
			entrySet |= USCXML_GET_STATE(i).completion;
			break;
		}

		case USCXML_STATE_HISTORY_SHALLOW:
		case USCXML_STATE_HISTORY_DEEP: {
			if (!BIT_HAS_AND(USCXML_GET_STATE(i).completion, _history) &&
			        !BIT_HAS(USCXML_GET_STATE(i).parent, _configuration)) {

				/* nothing set for history, look for a default transition */
				for (j = 0; j < _transitions.size(); j++) {
					if unlikely(USCXML_GET_TRANS(j).source == i) {
						entrySet |= USCXML_GET_TRANS(j).target;

						if(USCXML_STATE_MASK(USCXML_GET_STATE(i).type) == USCXML_STATE_HISTORY_DEEP &&
						        !BIT_HAS_AND(USCXML_GET_TRANS(j).target, USCXML_GET_STATE(i).children)) {
							for (k = i + 1; k < _states.size(); k++) {
								if (BIT_HAS(k, USCXML_GET_TRANS(j).target)) {
									entrySet |= USCXML_GET_STATE(k).ancestors;
									break;
								}
							}
						}
						BIT_SET_AT(j, transSet);
						break;
					}
					/* Note: SCXML mandates every history to have a transition! */
				}
			} else {
				tmpStates = USCXML_GET_STATE(i).completion;
				tmpStates &= _history;
				entrySet |= tmpStates;

				if (USCXML_GET_STATE(i).type == (USCXML_STATE_HAS_HISTORY | USCXML_STATE_HISTORY_DEEP)) {
					/* a deep history state with nested histories -> more completion */
					for (j = i + 1; j < USCXML_NUMBER_STATES; j++) {
						if (BIT_HAS(j, USCXML_GET_STATE(i).completion) &&
						        BIT_HAS(j, entrySet) &&
						        (USCXML_GET_STATE(j).type & USCXML_STATE_HAS_HISTORY)) {
							for (k = j + 1; k < USCXML_NUMBER_STATES; k++) {
								/* add nested history to entry_set */
								if ((USCXML_STATE_MASK(USCXML_GET_STATE(k).type) == USCXML_STATE_HISTORY_DEEP ||
								        USCXML_STATE_MASK(USCXML_GET_STATE(k).type) == USCXML_STATE_HISTORY_SHALLOW) &&
								        BIT_HAS(k, USCXML_GET_STATE(j).children)) {
									/* a nested history state */
									BIT_SET_AT(k, entrySet);
								}
							}
						}
					}
				}
			}
			break;
		}

		case USCXML_STATE_INITIAL: {
			for (j = 0; j < USCXML_NUMBER_TRANS; j++) {
				if (USCXML_GET_TRANS(j).source == i) {
					BIT_SET_AT(j, transSet);
					BIT_CLEAR(i, entrySet);
					entrySet |= USCXML_GET_TRANS(j).target;
					for (k = i + 1; k < USCXML_NUMBER_STATES; k++) {
						if (BIT_HAS(k, USCXML_GET_TRANS(j).target)) {
							entrySet |= USCXML_GET_STATE(k).ancestors;
						}
					}
				}
			}
			break;
		}
		case USCXML_STATE_COMPOUND: { /* we need to check whether one child is already in entry_set */
			if (!BIT_HAS_AND(entrySet, USCXML_GET_STATE(i).children) &&
			        (!BIT_HAS_AND(_configuration, USCXML_GET_STATE(i).children) ||
			         BIT_HAS_AND(exitSet, USCXML_GET_STATE(i).children))) {
				entrySet |= USCXML_GET_STATE(i).completion;
				if (!BIT_HAS_AND(USCXML_GET_STATE(i).completion, USCXML_GET_STATE(i).children)) {
					/* deep completion */
					for (j = i + 1; j < USCXML_NUMBER_STATES; j++) {
						if (BIT_HAS(j, USCXML_GET_STATE(i).completion)) {
							entrySet |= USCXML_GET_STATE(j).ancestors;
							break; /* completion of compound is single state */
						}
					}
				}
			}
			break;
		}
		}
		i = entrySet.find_next(i);

	}


#ifdef USCXML_VERBOSE
	std::cerr << "Transitions: " << transSet << std::endl;
#endif

	/* EXIT_STATES: */
	/* we cannot use find_first due to ordering */
	i = USCXML_NUMBER_STATES;
	while(i-- > 0) {
		if (BIT_HAS(i, exitSet) && BIT_HAS(i, _configuration)) {

			USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), beforeExitingState, USCXML_GET_STATE(i).element);

			/* call all on exit handlers */
			for (auto exitIter = USCXML_GET_STATE(i).onExit.begin(); exitIter != USCXML_GET_STATE(i).onExit.end(); exitIter++) {
				try {
					_callbacks->process(*exitIter);
				} catch (...) {
					// do nothing and continue with next block
				}
			}
			BIT_CLEAR(i, _configuration);

			USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), afterExitingState, USCXML_GET_STATE(i).element);

		}
	}

	/* TAKE_TRANSITIONS: */
	i = transSet.find_first();
	while(i != boost::dynamic_bitset<>::npos) {
		if ((USCXML_GET_TRANS(i).type & (USCXML_TRANS_HISTORY | USCXML_TRANS_INITIAL)) == 0) {
			USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), beforeTakingTransition, USCXML_GET_TRANS(i).element);

			if (USCXML_GET_TRANS(i).onTrans != NULL) {

				/* call executable content in non-history, non-initial transition */
				try {
					_callbacks->process(USCXML_GET_TRANS(i).onTrans);
				} catch (...) {
					// do nothing and continue with next block
				}
			}

			USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), afterTakingTransition, USCXML_GET_TRANS(i).element);

		}
		i = transSet.find_next(i);
	}

#ifdef USCXML_VERBOSE
	std::cerr << "Entering: ";
	printStateNames(entrySet);
#endif


	/* ENTER_STATES: */
	i = entrySet.find_first();
	while(i != boost::dynamic_bitset<>::npos) {

		if (BIT_HAS(i, _configuration)) {
			// already active
			i = entrySet.find_next(i);
			continue;
		}

		/* these are no proper states */
		if unlikely(USCXML_STATE_MASK(USCXML_GET_STATE(i).type) == USCXML_STATE_HISTORY_DEEP ||
		            USCXML_STATE_MASK(USCXML_GET_STATE(i).type) == USCXML_STATE_HISTORY_SHALLOW ||
		            USCXML_STATE_MASK(USCXML_GET_STATE(i).type) == USCXML_STATE_INITIAL) {
			i = entrySet.find_next(i);
			continue;
		}

		USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), beforeEnteringState, USCXML_GET_STATE(i).element);

		BIT_SET_AT(i, _configuration);

		/* initialize data */
		if (!BIT_HAS(i, _initializedData)) {
			for (auto dataIter = USCXML_GET_STATE(i).data.begin(); dataIter != USCXML_GET_STATE(i).data.end(); dataIter++) {
				_callbacks->initData(*dataIter);
			}
			BIT_SET_AT(i, _initializedData);
		}

		/* call all on entry handlers */
		for (auto entryIter = USCXML_GET_STATE(i).onEntry.begin(); entryIter != USCXML_GET_STATE(i).onEntry.end(); entryIter++) {
			try {
				_callbacks->process(*entryIter);
			} catch (...) {
				// do nothing and continue with next block
			}
		}

		USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), afterEnteringState, USCXML_GET_STATE(i).element);

		/* take history and initial transitions */
		for (j = 0; j < USCXML_NUMBER_TRANS; j++) {
			if unlikely(BIT_HAS(j, transSet) &&
			            (USCXML_GET_TRANS(j).type & (USCXML_TRANS_HISTORY | USCXML_TRANS_INITIAL)) &&
			            USCXML_GET_STATE(USCXML_GET_TRANS(j).source).parent == i) {

				USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), beforeTakingTransition, USCXML_GET_TRANS(j).element);

				/* call executable content in transition */
				if (USCXML_GET_TRANS(j).onTrans != NULL) {
					try {
						_callbacks->process(USCXML_GET_TRANS(j).onTrans);
					} catch (...) {
						// do nothing and continue with next block
					}
				}

				USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(), afterTakingTransition, USCXML_GET_TRANS(j).element);

			}
		}

		/* handle final states */
		if unlikely(USCXML_STATE_MASK(USCXML_GET_STATE(i).type) == USCXML_STATE_FINAL) {
			if unlikely(USCXML_GET_STATE(i).ancestors.count() == 1 && BIT_HAS(0, USCXML_GET_STATE(i).ancestors)) {
				// only the topmost scxml is an ancestor
				_flags |= USCXML_CTX_TOP_LEVEL_FINAL;
			} else {
				/* raise done event */
				_callbacks->raiseDoneEvent(USCXML_GET_STATE(USCXML_GET_STATE(i).parent).element, USCXML_GET_STATE(i).doneData);
			}

			/**
			 * are we the last final state to leave a parallel state?:
			 * 1. Gather all parallel states in our ancestor chain
			 * 2. Find all states for which these parallels are ancestors
			 * 3. Iterate all active final states and remove their ancestors
			 * 4. If a state remains, not all children of a parallel are final
			 */
			for (j = 0; j < USCXML_NUMBER_STATES; j++) {
				if unlikely(USCXML_STATE_MASK(USCXML_GET_STATE(j).type) == USCXML_STATE_PARALLEL &&
				            BIT_HAS(j, USCXML_GET_STATE(i).ancestors)) {
					tmpStates.reset();
					k = _configuration.find_first();
					while (k != boost::dynamic_bitset<>::npos) {
						if (BIT_HAS(j, USCXML_GET_STATE(k).ancestors)) {
							if (USCXML_STATE_MASK(USCXML_GET_STATE(k).type) == USCXML_STATE_FINAL) {
								tmpStates ^= USCXML_GET_STATE(k).ancestors;
							} else {
								BIT_SET_AT(k, tmpStates);
							}
						}
						k = _configuration.find_next(k);
					}
					if (!tmpStates.any()) {
						// raise done for state j
						_callbacks->raiseDoneEvent(USCXML_GET_STATE(j).element, USCXML_GET_STATE(j).doneData);
					}
				}
			}
		}
	}
	USCXML_MONITOR_CALLBACK(_callbacks->getMonitor(), afterMicroStep);

	// are we running in circles?
	if (_microstepConfigurations.find(_configuration) != _microstepConfigurations.end()) {
		USCXML_MONITOR_CALLBACK1(_callbacks->getMonitor(),
		                         reportIssue,
		                         InterpreterIssue("Reentering same configuration during microstep  - possible endless loop",
		                                 NULL,
		                                 InterpreterIssue::USCXML_ISSUE_WARNING));
	}
	_microstepConfigurations.insert(_configuration);

	return USCXML_MICROSTEPPED;
}

void FastMicroStep::reset() {
	_isCancelled = false;
	_flags = USCXML_CTX_PRISTINE;
	_configuration.reset();
	_history.reset();
	_initializedData.reset();
	_invocations.reset();

}

bool FastMicroStep::isInState(const std::string& stateId) {
#ifdef USCXML_VERBOSE
	printStateNames(_configuration);
#endif
	if (_stateIds.find(stateId) == _stateIds.end())
		return false;
	return _configuration[_stateIds[stateId]];
}

std::list<XERCESC_NS::DOMElement*> FastMicroStep::getConfiguration() {
	std::list<XERCESC_NS::DOMElement*> config;
	size_t i = _configuration.find_first();
	while(i != boost::dynamic_bitset<>::npos) {
		config.push_back(_states[i]->element);
		i = _configuration.find_next(i);
	}
	return config;
}


std::list<DOMElement*> FastMicroStep::getHistoryCompletion(const DOMElement* history) {
	std::set<std::string> elements;
	elements.insert(_xmlPrefix.str() + "history");
	std::list<DOMElement*> histories = DOMUtils::inPostFixOrder(elements, _scxml);

	std::list<DOMElement*> covered;
	std::list<DOMElement*> perParentcovered;
	const DOMNode* parent = NULL;

	std::list<DOMElement*> completion;


	if (parent != history->getParentNode()) {
		covered.insert(covered.end(), perParentcovered.begin(), perParentcovered.end());
		perParentcovered.clear();
		parent = history->getParentNode();
	}

	bool deep = (HAS_ATTR(history, "type") && iequals(ATTR(history, "type"), "deep"));

	for (size_t j = 0; j < _states.size(); j++) {
		if (_states[j]->element == history)
			continue;

		if (DOMUtils::isDescendant((DOMNode*)_states[j]->element, history->getParentNode()) && isHistory(_states[j]->element)) {
			((DOMElement*)history)->setUserData(X("hasHistoryChild"), _states[j], NULL);
		}

		if (DOMUtils::isMember(_states[j]->element, covered))
			continue;

		if (deep) {
			if (DOMUtils::isDescendant(_states[j]->element, history->getParentNode()) && !isHistory(_states[j]->element)) {
				completion.push_back(_states[j]->element);
			}
		} else {
			if (_states[j]->element->getParentNode() == history->getParentNode() && !isHistory(_states[j]->element)) {
				completion.push_back(_states[j]->element);
			}
		}
	}

	return completion;
}

#ifdef USCXML_VERBOSE
/**
 * Print name of states contained in a (debugging).
 */
void FastMicroStep::printStateNames(const boost::dynamic_bitset<>& a) {
	size_t i;
	const char* seperator = "";
	for (i = 0; i < a.size(); i++) {
		if (BIT_HAS(i, a)) {
			std::cerr << seperator << (HAS_ATTR(USCXML_GET_STATE(i).element, X("id")) ? ATTR(USCXML_GET_STATE(i).element, X("id")) : "UNK");
			seperator = ", ";
		}
	}
	std::cerr << std::endl;
}
#endif

std::list<DOMElement*> FastMicroStep::getCompletion(const DOMElement* state) {

	if (isHistory(state)) {
		// we already did in setHistoryCompletion
		return getHistoryCompletion(state);

	} else if (isParallel(state)) {
		return getChildStates(state);

	} else if (HAS_ATTR(state, "initial")) {
		return getStates(tokenize(ATTR(state, "initial")), _scxml);

	} else {
		std::list<DOMElement*> completion;

		std::list<DOMElement*> initElems = DOMUtils::filterChildElements(_xmlPrefix.str() + "initial", state);
		if(initElems.size() > 0) {
			// initial element is first child
			completion.push_back(initElems.front());
		} else {
			// first child state
			DOMNodeList* children = state->getChildNodes();
			for (size_t i = 0; i < children->getLength(); i++) {
				if (children->item(i)->getNodeType() != DOMNode::ELEMENT_NODE)
					continue;
				if (isState(dynamic_cast<DOMElement*>(children->item(i)))) {
					completion.push_back(dynamic_cast<DOMElement*>(children->item(i)));
					break;
				}
			}
		}
		return completion;
	}

}


#if 0
/**
 * See: http://www.w3.org/TR/scxml/#LegalStateConfigurations
 */
bool FastMicroStep::hasLegalConfiguration() {

	// The configuration contains exactly one child of the <scxml> element.
	std::list<DOMElement*> scxmlChilds = getChildStates(_scxml, true);
	DOMNode* foundScxmlChild;
	for (auto sIter = scxmlChilds.begin(); sIter != scxmlChilds.end(); sIter++) {
		DOMElement* state = *sIter;
		if (isMember(state, config)) {
			if (foundScxmlChild) {
				LOG(ERROR) << "Invalid configuration: Multiple childs of scxml root are active '" << ATTR_CAST(foundScxmlChild, "id") << "' and '" << ATTR_CAST(scxmlChilds[i], "id") << "'";
				return false;
			}
			foundScxmlChild = scxmlChilds[i];
		}
	}
	if (!foundScxmlChild) {
		LOG(ERROR) << "Invalid configuration: No childs of scxml root are active";

		return false;
	}

	// The configuration contains one or more atomic states.
	bool foundAtomicState = false;
	for (size_t i = 0; i < config.size(); i++) {
		if (isAtomic(Element<std::string>(config[i]))) {
			foundAtomicState = true;
			break;
		}
	}
	if (!foundAtomicState) {
		LOG(ERROR) << "Invalid configuration: No atomic state is active";
		return false;
	}

	// the configuration contains no history pseudo-states
	for (size_t i = 0; i < config.size(); i++) {
		if (isHistory(Element<std::string>(config[i]))) {
			LOG(ERROR) << "Invalid configuration: history state " << ATTR_CAST(config[i], "id") << " is active";
			return false;
		}
	}



	// When the configuration contains an atomic state, it contains all of its <state> and <parallel> ancestors.
	for (size_t i = 0; i < config.size(); i++) {
		if (isAtomic(Element<std::string>(config[i]))) {
			Node<std::string> parent = config[i];
			while(((parent = parent.getParentNode()) && parent.getNodeType() == Node_base::ELEMENT_NODE)) {
				if (isState(Element<std::string>(parent)) &&
				        (iequals(LOCALNAME(parent), "state") ||
				         iequals(LOCALNAME(parent), "parallel"))) {
					if (!isMember(parent, config)) {
						LOG(ERROR) << "Invalid configuration: atomic state '" << ATTR_CAST(config[i], "id") << "' is active, but parent '" << ATTR_CAST(parent, "id") << "' is not";
						return false;
					}
				}
			}
		}
	}

	// When the configuration contains a non-atomic <state>, it contains one and only one of the state's children
	for (size_t i = 0; i < config.size(); i++) {
		Element<std::string> configElem(config[i]);
		if (!isAtomic(configElem) && !isParallel(configElem)) {
			Node<std::string> foundChildState;
			//std::cerr << config[i] << std::endl;
			NodeSet<std::string> childs = getChildStates(config[i]);
			for (size_t j = 0; j < childs.size(); j++) {
				//std::cerr << childs[j] << std::endl;
				if (isMember(childs[j], config)) {
					if (foundChildState) {
						LOG(ERROR) << "Invalid configuration: Multiple childs of compound '" << ATTR_CAST(config[i], "id")
						           << "' are active '" << ATTR_CAST(foundChildState, "id") << "' and '" << ATTR_CAST(childs[j], "id") << "'";
						return false;
					}
					foundChildState = childs[j];
				}
			}
			if (!foundChildState) {
				LOG(ERROR) << "Invalid configuration: No childs of compound '" << ATTR_CAST(config[i], "id") << "' are active";
				return false;
			}
		}
	}

	// If the configuration contains a <parallel> state, it contains all of its children
	for (size_t i = 0; i < config.size(); i++) {
		if (isParallel(Element<std::string>(config[i]))) {
			NodeSet<std::string> childs = getChildStates(config[i]);
			for (size_t j = 0; j < childs.size(); j++) {
				if (!isMember(childs[j], config) && !isHistory(Element<std::string>(childs[j]))) {
					LOG(ERROR) << "Invalid configuration: Not all children of parallel '" << ATTR_CAST(config[i], "id") << "' are active i.e. '" << ATTR_CAST(childs[j], "id") << "' is not";
					return false;
				}
			}
		}
	}

	// everything worked out fine!
	return true;
}
#endif

}