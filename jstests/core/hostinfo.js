// The test runs commands that are not allowed with security token: hostInfo.
// @tags: [
//   not_allowed_with_security_token,
//     # `hostInfo` command is not available on embedded
//     incompatible_with_embedded,
// ]
// SERVER-4615:  Ensure hostInfo() command returns expected results on each platform

(function() {
"use strict";
assert.commandWorked(db.hostInfo());
var hostinfo = db.hostInfo();

// test for os-specific fields
if (hostinfo.os.type == "Windows") {
    assert.neq(hostinfo.os.name, "" || null, "Missing Windows os name");
    assert.neq(hostinfo.os.version, "" || null, "Missing Windows version");

} else if (hostinfo.os.type == "Linux") {
    assert.neq(hostinfo.os.name, "" || null, "Missing Linux os/distro name");
    assert.neq(hostinfo.os.version, "" || null, "Missing Lindows version");

} else if (hostinfo.os.type == "Darwin") {
    assert.neq(hostinfo.os.name, "" || null, "Missing Darwin os name");
    assert.neq(hostinfo.os.version, "" || null, "Missing Darwin version");

} else if (hostinfo.os.type == "BSD") {
    assert.neq(hostinfo.os.name, "" || null, "Missing FreeBSD os name");
    assert.neq(hostinfo.os.version, "" || null, "Missing FreeBSD version");
}

jsTest.log(hostinfo);
// comment out this block for systems which have not implemented hostinfo.
if (hostinfo.os.type != "") {
    assert.neq(hostinfo.system.hostname, "" || null, "Missing Hostname");
    assert.neq(hostinfo.system.currentTime, "" || null, "Missing Current Time");
    assert.neq(hostinfo.system.cpuAddrSize, "" || null || 0, "Missing CPU Address Size");
    assert.neq(hostinfo.system.memSizeMB, "" || null, "Missing Memory Size");
    assert.neq(hostinfo.system.numCores, "" || null || 0, "Missing Number of Cores");
    assert.neq(
        hostinfo.system.numPhysicalCores, "" || null || 0, "Missing Number of Physical Cores");
    assert.neq(hostinfo.system.numCpuSockets, "" || null || 0, "Missing Number of CPU Sockets");
    assert.neq(hostinfo.system.cpuArch, "" || null, "Missing CPU Architecture");
    assert.neq(hostinfo.system.numaEnabled, "" || null, "Missing NUMA flag");
    assert.neq(hostinfo.system.numNumaNodes, "" || null || 0, "Missing Number of NUMA Nodes");
}

var buildInfo = assert.commandWorked(db.runCommand({buildInfo: 1}));
if (buildInfo.buildEnvironment && buildInfo.buildEnvironment.target_arch) {
    let targetArch = buildInfo.buildEnvironment.target_arch;
    if (targetArch == "i386")
        assert.eq(hostinfo.system.cpuAddrSize, 32);
    else
        assert.eq(hostinfo.system.cpuAddrSize, 64);
    assert.eq(hostinfo.system.cpuAddrSize, buildInfo.bits);
}
})();
