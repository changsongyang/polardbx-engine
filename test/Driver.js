/*
 Copyright (c) 2012, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */

require("./test_config.js");

var fs = require("fs"),
    util = require('util');


/** Driver 
*/
function Driver() {
  this.suites = [];
  this.fileToRun = "";
  this.suitesToRun = "";
};

Driver.prototype.findSuites = function(directory) {
  if(this.fileToRun) {
    var pathname = path.join(directory, this.fileToRun);
    this.suites.push(new harness.Suite("file", pathname));
  }
  else { 
    /* Read the test directory, building list of suites */
    var files = fs.readdirSync(directory);
    for(var i = 0; i < files.length ; i++) {
      var f = files[i];
      var st = fs.statSync(path.join(directory, f));
      if (st.isDirectory() && this.isSuiteToRun(f)) {
        if (debug) console.log('Driver.findSuites found directory ' + f);
        this.suites.push(new harness.Suite(f, path.join(directory, f)));
      }
    }
  }
};

Driver.prototype.isSuiteToRun = function(directoryName) {
  if(directoryName == 'lib') return false;
  if (debug) console.log("SuitesToRun: " + this.suitesToRun);
  if (this.suitesToRun && this.suitesToRun.indexOf(directoryName) == -1) {
    if (debug) console.log('Driver.isSuiteToRun for ' + directoryName + ' returns false.');
    return false;
  }
   if (debug) console.log('Driver.isSuiteToRun for ' + directoryName + ' returns true.');
  return true;
};

Driver.prototype.testCompleted = function(testCase) {
  if (debug) console.log('Driver.testCompleted: ' + testCase.name);
  var suite = testCase.suite;
  if (debug) console.log('Driver.testCompleted suite for ' + suite.name);
  if (suite.testCompleted(testCase)) {
    // this suite is done; remove it from the list of running suites
    if (--this.numberOfRunningSuites == 0) {
      // no more running suites; report and exit
      this.reportResultsAndExit();
    }
  } 
};

Driver.prototype.reportResultsAndExit = function() {
  // close all session factories
  sessionFactories = mynode.getOpenSessionFactories();
  sessionFactories.forEach(function(sessionFactory) {
    if (debug) console.log('Driver.reportResultsAndExit closing ' + sessionFactory.key);
    sessionFactory.close();
  });
  console.log("Passed: ", result.passed.length);
  console.log("Failed: ", result.failed.length);
  process.exit(result.failed.length > 0);  
};


/*****************************************************************************
 ********************** main process *****************************************
 *****************************************************************************/

driver = new Driver();
var result = new harness.Result(driver);
result.listener = new harness.Listener();

var usageMessage = 
  "Usage: node driver [options]\n" +
  "                 -h: print this message\n" +
  "             --help: print this message\n" +
  "                 -d: set the debug flag\n" +
  "            --debug: set the debug flag\n" +
  "    --suite=<suite>: only run the named suite(s)\n" +
  "   --suites=<suite>: only run the named suite(s)\n" +
  "--adapter=<adapter>: set the mynode adapter (ndb or mysql)\n"
  ;

// handle command line arguments
process.argv.forEach(function (val, index, array) {
  if (index >= 2) { // ignore first two values (node driver)
    switch (val) {
    case '--debug':
    case '-d':
      console.log('Setting debug to true');
      debug = true;
      break;
    case '--help':
    case '-h':
      exit = true;
      break;
    default:
      values = val.split('=');
      if (values.length == 2) {
        switch (values[0]) {
        case '--adapter':
          global.adapter = values[1];
          break;
        case '--suite':
        case '--suites':
          driver.suitesToRun = values[1];
          break;
        case '--file':
          driver.fileToRun = values[1];
          break;
        default:
          console.log('Invalid option ' + val);
          exit = true;
        }
      } else {
        console.log('Invalid option ' + val);
        exit = true;
     }
    }
  }
});

if (exit) {
  console.log(usageMessage);
  process.exit(0);
}


driver.findSuites(__dirname);
if (debug) console.log('suites found ' + driver.suites.length);

driver.suites.forEach(function(suite) {
  if (debug) console.log('createTests for ' + suite.name);
  suite.createTests();
});

// now run tests
driver.numberOfRunningSuites = 0;
driver.suites.forEach(function(suite) {
  if (debug) console.log('main running tests for ' + suite.name);
  if (suite.runTests(result)) {
    driver.numberOfRunningSuites++;
  }
});

// if we did not start any suites, exit now
if (driver.numberOfRunningSuites == 0) {
  driver.reportResultsAndExit();
}

var timeoutMillis = 10000;
// set a timeout to prevent process from waiting forever
if (debug) console.log('Setting timeout of ' + timeoutMillis);
  setTimeout(function() {
    var nwait = result.listener.started - result.listener.ended;
    var tests = (nwait == 1 ? " test." : " tests.");
    console.log('TIMEOUT: still waiting for ' + nwait + tests);
  }, timeoutMillis);
