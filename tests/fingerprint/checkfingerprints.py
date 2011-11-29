import argparse
import os
import re
import subprocess
import sys
import time
import unittest

inetRoot = os.path.abspath("../..")
sep = ";" if sys.platform == 'win32' else ':'
nedPath = inetRoot + "/src" + sep + inetRoot + "/examples" + sep + inetRoot + "/tests/fingerprint"
inetLib = inetRoot + "/src/inet"
opp_run = "opp_run"
cpuTimeLimit = "10s"
logFile = "test.out"


_lastComputedFingerprint = None  # FIXME khmm...

def iif(cond,t,f):
    return t if cond else f
    
class FingerprintTestCaseGenerator():
    def generateFromCSV(self, csvFileList, filterRegexList):
        testcases = []
        for csvFile in csvFileList:
            f = open(csvFile, 'r')
            contents = f.read()
            f.close()
            simulations = self.parseSimulationsTable(contents)
            testcases.extend(self.generateFromDictList(simulations, filterRegexList))
        return testcases

    def generateFromDictList(self, simulations, filterRegexList):
        testcases = []
        for simulation in simulations:
            title = simulation['wd'] + " " + simulation['args']
            if not filterRegexList or ['x' for regex in filterRegexList if re.search(regex, title)]: # if any regex matches title
                testcases.append(FingerprintTestCase(title, simulation['wd'], simulation['args'], simulation['simtimelimit'], simulation['fingerprint']))
        return testcases

    # parse the CSV into a list of dicts
    def parseSimulationsTable(self, text):
        simulations = []
        for line in text.splitlines():
            line = line.strip()
            if line != "" and not line.startswith("#"):
                fields = re.split(", +", line)
                if len(fields) != 4:
                    raise Exception("Line must contain 4 items: " + line)
                simulations.append({'wd': fields[0], 'args': fields[1], 'simtimelimit': fields[2], 'fingerprint': fields[3]})
        return simulations
    
    def formatUpdatedSimulationsTable(self, simulations):
        # if there is a computedFingerprint, use that instead of fingerprint
        txt = "# workingdir".ljust(35) + ", " + "args".ljust(45) + ", " + "simtimelimit".ljust(15) + ", " + "fingerprint\n"
        for simulation in simulations:
            line = simulation['wd'].ljust(35) + ", " + simulation['args'].ljust(45) + ", " + simulation['simtimelimit'].ljust(15) + ", " + \
                (simulation['computedFingerprint'] if "computedFingerprint" in simulation else simulation['fingerprint'])
            txt += line + "\n"
        txt = re.sub("( +),", ",\\1", txt)
        return txt

class SimulationResult:
    def __init__(self, command, workingdir, exitcode, errorMsg=None, isFingerprintOK=None, computedFingerprint=None, simulatedTime=None, numEvents=None, elapsedTime=None, cpuTimeLimitReached=None):
        self.command = command
        self.workingdir = workingdir
        self.exitcode = exitcode
        self.errorMsg = errorMsg
        self.isFingerprintOK = isFingerprintOK
        self.computedFingerprint = computedFingerprint
        self.simulatedTime = simulatedTime
        self.numEvents = numEvents
        self.elapsedTime = elapsedTime
        self.cpuTimeLimitReached = cpuTimeLimitReached

class SimulationTestCase(unittest.TestCase):
    def runSimulation(self, title, command, workingdir):
        global logFile
        
        # run the program and log the output
        FILE = open(logFile, "a")
        FILE.write("------------------------------------------------------\n")
        FILE.write("Running: " + title + "\n\n")
        FILE.write("$ cd " + workingdir + "\n");
        FILE.write("$ " + command + "\n\n");
        t0 = time.clock()
        (exitcode, out) = self.runProgram(command, workingdir)
        elapsedTime = time.clock() - t0
        FILE.write(out.strip() + "\n\n")
        FILE.write("Exit code: " + str(exitcode) + "\n")
        FILE.write("Elapsed time:  " + str(round(elapsedTime,2)) + "s\n\n")
        FILE.close()
    
        result = SimulationResult(command, workingdir, exitcode, elapsedTime=elapsedTime)
    
        # process error messages
        errorLines = re.findall("<!>.*", out, re.M)
        errorMsg = ""
        for err in errorLines:
            err = err.strip()
            if re.search("Fingerprint", err):
                if re.search("successfully", err):
                    result.isFingerprintOK = True
                else:
                    m = re.search("(computed|calculated): ([-a-zA-Z0-9]+)", err)
                    if m:
                        result.isFingerprintOK = False
                        result.computedFingerprint = m.group(2)
                    else:
                        raise Exception("Cannot parse fingerprint-related error message: " + err)
            else:
                errorMsg += "\n" + err
                if re.search("CPU time limit reached", err):
                    result.cpuTimeLimitReached = True
                m = re.search("at event #([0-9]+), t=([0-9]*(\\.[0-9]+)?)", err)
                if m:
                    result.numEvents = int(m.group(1))
                    result.simulatedTime = float(m.group(2))
    
        result.errormsg = errorMsg.strip()
        return result
    
    def runProgram(self, command, workingdir):
        process = subprocess.Popen(command, shell=True, cwd=workingdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = process.communicate()[0]
        out = re.sub("\r", "", out)
        return (process.returncode, out)

class FingerprintTestCase(SimulationTestCase):
    def __init__(self, title, wd, args, simtimelimit, fingerprint):
        SimulationTestCase.__init__(self)
        self.title = title 
        self.wd = wd
        self.args = args
        self.simtimelimit = simtimelimit 
        self.fingerprint = fingerprint

    def runTest(self):
        # CPU time limit is a safety guard: fingerprint checks shouldn't take forever
        global _lastComputedFingerprint, inetRoot, opp_run, nedPath, cpuTimeLimit
    
        # run the simulation
        workingdir = iif(self.wd.startswith('/'), inetRoot + "/" + self.wd, self.wd)
        command = opp_run + " -n " + nedPath + " -l " + inetLib + " -u Cmdenv " + self.args + \
            iif(self.simtimelimit!="", " --sim-time-limit=" + self.simtimelimit, "") + \
            " --fingerprint=" + self.fingerprint + " --cpu-time-limit=" + cpuTimeLimit
    
        result = self.runSimulation(self.title, command, workingdir)
    
        # process the result
        # note: fingerprint mismatch is technically NOT an error in 4.2 or before! (exitcode==0)
        if result.exitcode != 0:
            raise Exception(result.errormsg)
        elif result.cpuTimeLimitReached:
            raise Exception("cpu time limit exceeded")
        elif result.simulatedTime == 0:
            raise Exception("zero time simulated")
        elif result.isFingerprintOK is None:
            raise Exception("other")
        elif result.isFingerprintOK == False:
            _lastComputedFingerprint = result.computedFingerprint
            assert False, "fingerprint mismatch; actual: " + str(result.computedFingerprint)
        else:
            pass
        
    def __str__(self):
        return self.title

#
# Copy/paste of TextTestResult, with minor modifications in the output:
# we want to print the error text after ERROR and FAIL, but we don't want
# to print stack traces.  
#
class SimulationTextTestResult(unittest.TestResult):
    """A test result class that can print formatted text results to a stream.

    Used by TextTestRunner.
    """
    separator1 = '=' * 70
    separator2 = '-' * 70

    def __init__(self, stream, descriptions, verbosity):
        super(SimulationTextTestResult, self).__init__()
        self.stream = stream
        self.showAll = verbosity > 1
        self.dots = verbosity == 1
        self.descriptions = descriptions

    def getDescription(self, test):
        doc_first_line = test.shortDescription()
        if self.descriptions and doc_first_line:
            return '\n'.join((str(test), doc_first_line))
        else:
            return str(test)

    def startTest(self, test):
        super(SimulationTextTestResult, self).startTest(test)
        if self.showAll:
            self.stream.write(self.getDescription(test))
            self.stream.write(" ... ")
            self.stream.flush()

    def addSuccess(self, test):
        super(SimulationTextTestResult, self).addSuccess(test)
        if self.showAll:
            self.stream.writeln("ok")
        elif self.dots:
            self.stream.write('.')
            self.stream.flush()

    def addError(self, test, err):
        # modified
        super(SimulationTextTestResult, self).addError(test, err)
        self.errors[-1] = (test, err[1])  # super class method inserts stack trace; we don't need that, so overwrite it
        if self.showAll:
            self.stream.writeln("ERROR (%s)" % err[1])
        elif self.dots:
            self.stream.write('E')
            self.stream.flush()

    def addFailure(self, test, err):
        # modified
        super(SimulationTextTestResult, self).addFailure(test, err)
        self.failures[-1] = (test, err[1])  # super class method inserts stack trace; we don't need that, so overwrite it
        if self.showAll:
            self.stream.writeln("FAIL (%s)" % err[1])
        elif self.dots:
            self.stream.write('F')
            self.stream.flush()

    def addSkip(self, test, reason):
        super(SimulationTextTestResult, self).addSkip(test, reason)
        if self.showAll:
            self.stream.writeln("skipped {0!r}".format(reason))
        elif self.dots:
            self.stream.write("s")
            self.stream.flush()

    def addExpectedFailure(self, test, err):
        super(SimulationTextTestResult, self).addExpectedFailure(test, err)
        if self.showAll:
            self.stream.writeln("expected failure")
        elif self.dots:
            self.stream.write("x")
            self.stream.flush()

    def addUnexpectedSuccess(self, test):
        super(SimulationTextTestResult, self).addUnexpectedSuccess(test)
        if self.showAll:
            self.stream.writeln("unexpected success")
        elif self.dots:
            self.stream.write("u")
            self.stream.flush()

    def printErrors(self):
        # modified
        if self.dots or self.showAll:
            self.stream.writeln()
        self.printErrorList('Errors', self.errors)
        self.printErrorList('Failures', self.failures)

    def printErrorList(self, flavour, errors):
        # modified
        if errors:
            self.stream.writeln("%s:" % flavour)
        for test, err in errors:
            self.stream.writeln("  %s: %s" % (self.getDescription(test), err))

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description='Run the fingerprint tests specified in the input files.')
    parser.add_argument('testspecfile', nargs='+', help='CSV files that contain the tests to run. Columns: workingdir,args,simtimelimit,fingerprint')
    parser.add_argument('-m', '--match', nargs='*', metavar='RE', help='Line filter: a line (more precisely, workingdir+SPACE+args) must match any of the regular expressions in order for that test case to be run')
    args = parser.parse_args()

    if os.path.isfile(logFile):
        os.unlink(logFile)
        
    testcases = FingerprintTestCaseGenerator().generateFromCSV(args.testspecfile, args.match)
    
    testSuite = unittest.TestSuite()
    testSuite.addTests(testcases)

    testRunner = unittest.TextTestRunner(verbosity=9, resultclass=SimulationTextTestResult)
    
    testRunner.run(testSuite)

#        updatedContents = formatUpdatedSimulationsTable(simulations)
#        if contents != updatedContents:
#            updatedInputFile = inputFile + ".updated"
#            ff = open(updatedInputFile, 'w')
#            ff.write(updatedContents)
#            ff.close()
#            print "Check " + updatedInputFile + " for updated fingerprints"

