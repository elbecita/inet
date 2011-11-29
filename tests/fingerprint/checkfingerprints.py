import argparse
import os
import re
import subprocess
import sys
import time

logFile = "test.out"
inetRoot = os.path.abspath("../..")
sep = ";" if sys.platform == 'win32' else ':'
nedPath = inetRoot + "/src" + sep + inetRoot + "/examples" + sep + inetRoot + "/tests/fingerprint"
inetLib = inetRoot + "/src/inet"
opp_run = "opp_run"

def main():
    parser = argparse.ArgumentParser(description='Run the fingerprint tests specified in the input files.')
    parser.add_argument('testspecfile', nargs='+', help='CSV files that contain the tests to run. Columns: workingdir,args,simtimelimit,fingerprint')
    parser.add_argument('-m', '--match', nargs='*', metavar='RE', help='Line filter: a line (more precisely, workingdir+SPACE+args) must match any of the regular expressions in order for that test case to be run')
    args = parser.parse_args()

    if os.path.isfile(logFile):
        os.unlink(logFile)

    passList = []
    failList = []
    errorList = []

    # run the tests in each input file
    for inputFile in args.testspecfile:
        f = open(inputFile, 'r')
        contents = f.read()
        f.close()

        simulations = parseSimulationsTable(contents)

        (passes, fails, errors) = testAll(simulations, args.match)

        passList.extend(passes)
        failList.extend(fails)
        errorList.extend(errors)

        updatedContents = formatUpdatedSimulationsTable(simulations)
        if contents != updatedContents:
            updatedInputFile = inputFile + ".updated"
            ff = open(updatedInputFile, 'w')
            ff.write(updatedContents)
            ff.close()
            print "Check " + updatedInputFile + " for updated fingerprints"

    # report results
    print "\nPASS:", len(passList), "   FAIL:", len(failList), "   ERROR:", len(errorList)
    if failList:
        print "Failures:\n  " + "\n  ".join(failList)
    if errorList:
        print "Errors:\n  " + "\n  ".join(errorList)

_lastComputedFingerprint = None  # khmm...

def testAll(simulations, regexList):
    global _lastComputedFingerprint

    passList = []
    failList = []
    errorList = []

    for simulation in simulations:
        title = simulation['wd'] + " " + simulation['args']
        if not regexList or ['x' for regex in regexList if re.search(regex, title)]: # if any regex matches title
            try:
                print title + ":",
                runTest(title, simulation['wd'], simulation['args'], simulation['simtimelimit'], simulation['fingerprint'])
                passList.append(title)
                print "PASS"
            except AssertionError:
                failList.append(title)
                print "FAIL (%s)" % sys.exc_info()[1]
                if _lastComputedFingerprint is not None:
                    simulation['computedFingerprint'] = _lastComputedFingerprint
            except:
                errorList.append(title)
                print "ERROR (%s)" % sys.exc_info()[1]

    return (passList, failList, errorList)

def runTest(title, wd, args, simtimelimit, fingerprint):
    global _lastComputedFingerprint

    # run the simulation
    workingdir = iif(wd.startswith('/'), inetRoot + "/" + wd, wd)
    command = opp_run + " -n " + nedPath + " -l " + inetLib + " -u Cmdenv " + args + \
        iif(simtimelimit!="", " --sim-time-limit=" + simtimelimit, "") + \
        " --fingerprint=" + fingerprint + " --cpu-time-limit=10s"  # CPU time limit is a safety guard: fingerprint checks shouldn't take forever

    result = runSimulation(title, command, workingdir)

    # process the result
    # note: fingerprint mismatch is technically NOT an error in 4.2 or before! (exitcode==0)
    if result.exitcode != 0:
        raise Exception(result.errormsg.strip())
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

# parse the CSV into a list of dicts
def parseSimulationsTable(text):
    simulations = []
    for line in text.splitlines():
        line = line.strip()
        if line != "" and not line.startswith("#"):
            fields = re.split(", +", line)
            if len(fields) != 4:
                raise Exception("Line must contain 4 items: " + line)
            simulations.append({'wd': fields[0], 'args': fields[1], 'simtimelimit': fields[2], 'fingerprint': fields[3]})
    return simulations

def formatUpdatedSimulationsTable(simulations):
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

def runSimulation(title, command, workingdir):
    # run the program and log the output
    FILE = open(logFile, "a")
    FILE.write("------------------------------------------------------\n")
    FILE.write("Running: " + title + "\n\n")
    FILE.write("$ cd " + workingdir + "\n");
    FILE.write("$ " + command + "\n\n");
    t0 = time.clock()
    (exitcode, out) = runProgram(command, workingdir)
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

def runProgram(command, workingdir):
    process = subprocess.Popen(command, shell=False, cwd=workingdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = process.communicate()[0]
    out = re.sub("\r", "", out)
    return (process.returncode, out)

def iif(cond,t,f):
    return t if cond else f

main()
