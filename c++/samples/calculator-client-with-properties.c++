// Copyright (c) 2013, Kenton Varda <temporal@gmail.com>
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
// ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "calculator.capnp.h"
#include <capnp/ez-rpc.h>
#include <kj/debug.h>
#include <math.h>
#include <iostream>

class PowerFunction final: public Calculator::Function::Server {
  // An implementation of the Function interface wrapping pow().  Note that
  // we're implementing this on the client side and will pass a reference to
  // the server.  The server will then be able to make calls back to the client.

public:
  kj::Promise<void> call(CallContext context) {
    auto params = *context.getParams().params;
    KJ_REQUIRE(params.size() == 2, "Wrong number of parameters.");
    context.getResults().value = pow(params[0], params[1]);
    return kj::READY_NOW;
  }
};

int main(int argc, const char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " HOST:PORT\n"
        "Connects to the Calculator server at the given address and "
        "does some RPCs." << std::endl;
    return 1;
  }

  capnp::EzRpcClient client(argv[1]);
  Calculator::Client calculator = client.importCap<Calculator>("calculator");

  // Keep an eye on `waitScope`.  Whenever you see it used is a place where we
  // stop and wait for the server to respond.  If a line of code does not use
  // `waitScope`, then it does not block!
  auto& waitScope = client.getWaitScope();

  {
    // Make a request that just evaluates the literal value 123.
    //
    // What's interesting here is that evaluate() returns a "Value", which is
    // another interface and therefore points back to an object living on the
    // server.  We then have to call read() on that object to read it.
    // However, even though we are making two RPC's, this block executes in
    // *one* network round trip because of promise pipelining:  we do not wait
    // for the first call to complete before we send the second call to the
    // server.

    std::cout << "Evaluating a literal... ";
    std::cout.flush();

    // Set up the request.
    auto request = calculator.evaluateRequest();
    request.expression->literal = 123;

    // Send it, which returns a promise for the result (without blocking).
    auto evalPromise = request.send();

    // Using the promise, create a pipelined request to call read() on the
    // returned object, and then send that.
    auto readPromise = evalPromise.value->readRequest().send();

    // Now that we've sent all the requests, wait for the response.  Until this
    // point, we haven't waited at all!
    auto response = readPromise.wait(waitScope);
    KJ_ASSERT(response.value == 123);

    std::cout << "PASS" << std::endl;
  }

  {
    // Make a request to evaluate 123 + 45 - 67.
    //
    // The Calculator interface requires that we first call getOperator() to
    // get the addition and subtraction functions, then call evaluate() to use
    // them.  But, once again, we can get both functions, call evaluate(), and
    // then read() the result -- four RPCs -- in the time of *one* network
    // round trip, because of promise pipelining.

    std::cout << "Using add and subtract... ";
    std::cout.flush();

    Calculator::Function::Client add = nullptr;
    Calculator::Function::Client subtract = nullptr;

    {
      // Get the "add" function from the server.
      auto request = calculator.getOperatorRequest();
      request.op = Calculator::Operator::ADD;
      add = *request.send().func;
    }

    {
      // Get the "subtract" function from the server.
      auto request = calculator.getOperatorRequest();
      request.op = Calculator::Operator::SUBTRACT;
      subtract = *request.send().func;
    }

    // Build the request to evaluate 123 + 45 - 67.
    auto request = calculator.evaluateRequest();

    auto subtractCall = request.expression->call.init();
    subtractCall.function = subtract;
    auto subtractParams = subtractCall.params.init(2);
    subtractParams[1].literal = 67;

    auto addCall = subtractParams[0].call.init();
    addCall.function = add;
    auto addParams = addCall.params.init(2);
    addParams[0].literal = 123;
    addParams[1].literal = 45;

    // Send the evaluate() request, read() the result, and wait for read() to
    // finish.
    auto evalPromise = request.send();
    auto readPromise = evalPromise.value->readRequest().send();

    auto response = readPromise.wait(waitScope);
    KJ_ASSERT(response.value == 101);

    std::cout << "PASS" << std::endl;
  }

  {
    // Make a request to evaluate 4 * 6, then use the result in two more
    // requests that add 3 and 5.
    //
    // Since evaluate() returns its result wrapped in a `Value`, we can pass
    // that `Value` back to the server in subsequent requests before the first
    // `evaluate()` has actually returned.  Thus, this example again does only
    // one network round trip.

    std::cout << "Pipelining eval() calls... ";
    std::cout.flush();

    Calculator::Function::Client add = nullptr;
    Calculator::Function::Client multiply = nullptr;

    {
      // Get the "add" function from the server.
      auto request = calculator.getOperatorRequest();
      request.op = Calculator::Operator::ADD;
      add = *request.send().func;
    }

    {
      // Get the "multiply" function from the server.
      auto request = calculator.getOperatorRequest();
      request.op = Calculator::Operator::MULTIPLY;
      multiply = *request.send().func;
    }

    // Build the request to evaluate 4 * 6
    auto request = calculator.evaluateRequest();

    auto multiplyCall = request.expression->call.init();
    multiplyCall.function = multiply;
    auto multiplyParams = multiplyCall.params.init(2);
    multiplyParams[0].literal = 4;
    multiplyParams[1].literal = 6;

    auto multiplyResult = *request.send().value;

    // Use the result in two calls that add 3 and add 5.

    auto add3Request = calculator.evaluateRequest();
    auto add3Call = add3Request.expression->call.init();
    add3Call.function = add;
    auto add3Params = add3Call.params.init(2);
    add3Params[0].previousResult = multiplyResult;
    add3Params[1].literal = 3;
    auto add3Promise = add3Request.send().value->readRequest().send();

    auto add5Request = calculator.evaluateRequest();
    auto add5Call = add5Request.expression->call.init();
    add5Call.function = add;
    auto add5Params = add5Call.params.init(2);
    add5Params[0].previousResult = multiplyResult;
    add5Params[1].literal = 5;
    auto add5Promise = add5Request.send().value->readRequest().send();

    // Now wait for the results.
    KJ_ASSERT(add3Promise.wait(waitScope).value == 27);
    KJ_ASSERT(add5Promise.wait(waitScope).value == 29);

    std::cout << "PASS" << std::endl;
  }

  {
    // Our calculator interface supports defining functions.  Here we use it
    // to define two functions and then make calls to them as follows:
    //
    //   f(x, y) = x * 100 + y
    //   g(x) = f(x, x + 1) * 2;
    //   f(12, 34)
    //   g(21)
    //
    // Once again, the whole thing takes only one network round trip.

    std::cout << "Defining functions... ";
    std::cout.flush();

    Calculator::Function::Client add = nullptr;
    Calculator::Function::Client multiply = nullptr;
    Calculator::Function::Client f = nullptr;
    Calculator::Function::Client g = nullptr;

    {
      // Get the "add" function from the server.
      auto request = calculator.getOperatorRequest();
      request.op = Calculator::Operator::ADD;
      add = *request.send().func;
    }

    {
      // Get the "multiply" function from the server.
      auto request = calculator.getOperatorRequest();
      request.op = Calculator::Operator::MULTIPLY;
      multiply = *request.send().func;
    }

    {
      // Define f.
      auto request = calculator.defFunctionRequest();
      request.paramCount = 2;

      {
        // Build the function body.
        auto addCall = request.body->call.init();
        addCall.function = add;
        auto addParams = addCall.params.init(2);
        addParams[1].parameter = 1;  // y

        auto multiplyCall = addParams[0].call.init();
        multiplyCall.function = multiply;
        auto multiplyParams = multiplyCall.params.init(2);
        multiplyParams[0].parameter = 0;  // x
        multiplyParams[1].literal = 100;
      }

      f = *request.send().func;
    }

    {
      // Define g.
      auto request = calculator.defFunctionRequest();
      request.paramCount = 1;

      {
        // Build the function body.
        auto multiplyCall = request.body->call.init();
        multiplyCall.function = multiply;
        auto multiplyParams = multiplyCall.params.init(2);
        multiplyParams[1].literal = 2;

        auto fCall = multiplyParams[0].call.init();
        fCall.function = f;
        auto fParams = fCall.params.init(2);
        fParams[0].parameter = 0;

        auto addCall = fParams[1].call.init();
        addCall.function = add;
        auto addParams = addCall.params.init(2);
        addParams[0].parameter = 0;
        addParams[1].literal = 1;
      }

      g = *request.send().func;
    }

    // OK, we've defined all our functions.  Now create our eval requests.

    // f(12, 34)
    auto fEvalRequest = calculator.evaluateRequest();
    auto fCall = fEvalRequest.expression.init().call.init();
    fCall.function = f;
    auto fParams = fCall.params.init(2);
    fParams[0].literal = 12;
    fParams[1].literal = 34;
    auto fEvalPromise = fEvalRequest.send().value->readRequest().send();

    // g(21)
    auto gEvalRequest = calculator.evaluateRequest();
    auto gCall = gEvalRequest.expression.init().call.init();
    gCall.function = g;
    gCall.params.init(1)[0].literal = 21;
    auto gEvalPromise = gEvalRequest.send().value->readRequest().send();

    // Wait for the results.
    KJ_ASSERT(fEvalPromise.wait(waitScope).value == 1234);
    KJ_ASSERT(gEvalPromise.wait(waitScope).value == 4244);

    std::cout << "PASS" << std::endl;
  }

  {
    // Make a request that will call back to a function defined locally.
    //
    // Specifically, we will compute 2^(4 + 5).  However, exponent is not
    // defined by the Calculator server.  So, we'll implement the Function
    // interface locally and pass it to the server for it to use when
    // evaluating the expression.
    //
    // This example requires two network round trips to complete, because the
    // server calls back to the client once before finishing.  In this
    // particular case, this could potentially be optimized by using a tail
    // call on the server side -- see CallContext::tailCall().  However, to
    // keep the example simpler, we haven't implemented this optimization in
    // the sample server.

    std::cout << "Using a callback... ";
    std::cout.flush();

    Calculator::Function::Client add = nullptr;

    {
      // Get the "add" function from the server.
      auto request = calculator.getOperatorRequest();
      request.op = Calculator::Operator::ADD;
      add = *request.send().func;
    }

    // Build the eval request for 2^(4+5).
    auto request = calculator.evaluateRequest();

    auto powCall = request.expression->call.init();
    powCall.function = kj::heap<PowerFunction>();
    auto powParams = powCall.params.init(2);
    powParams[0].literal = 2;

    auto addCall = powParams[1].call.init();
    addCall.function = add;
    auto addParams = addCall.params.init(2);
    addParams[0].literal = 4;
    addParams[1].literal = 5;

    // Send the request and wait.
    auto response = request.send().value->readRequest()
                           .send().wait(waitScope);
    KJ_ASSERT(response.value == 512);

    std::cout << "PASS" << std::endl;
  }

  return 0;
}
