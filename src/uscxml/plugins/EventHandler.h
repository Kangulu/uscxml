/**
 *  @file
 *  @author     2012-2014 Stefan Radomski (stefan.radomski@cs.tu-darmstadt.de)
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

#ifndef EVENTHANDLER_H_2801243E
#define EVENTHANDLER_H_2801243E

#include "uscxml/Common.h"
#include "uscxml/messages/Data.h"
#include "uscxml/messages/Event.h"

#include <list>
#include <string>
#include <memory>
#include <sstream>

#include <xercesc/dom/DOM.hpp>

namespace uscxml {

class InterpreterImpl;

class USCXML_API EventHandlerImpl {
public:
	EventHandlerImpl() {}
	virtual ~EventHandlerImpl() {}

	virtual std::list<std::string> getNames() = 0;
	virtual Data getDataModelVariables() = 0;

protected:
	InterpreterImpl* _interpreter;
};

class USCXML_API EventHandler {
public:
	PIMPL_OPERATORS(EventHandler);

	virtual std::list<std::string> getNames()   {
		return _impl->getNames();
	}

	virtual Data getDataModelVariables() const {
		return _impl->getDataModelVariables();
	};

protected:
	std::shared_ptr<EventHandlerImpl> _impl;
	friend class InterpreterImpl;
};


}


#endif /* end of include guard: EVENTHANDLER_H_2801243E */
