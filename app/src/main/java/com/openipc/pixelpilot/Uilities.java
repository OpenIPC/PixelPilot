package com.openipc.pixelpilot;

import android.util.Log;

import com.jcraft.jsch.ChannelExec;
import com.jcraft.jsch.JSch;
import com.jcraft.jsch.Session;

public class Uilities {

    public static boolean syncTimeToRemote(String sshHost, int sshPort, String sshUser, String sshPassword) {
        try {
            JSch jsch = new JSch();
            Session session = jsch.getSession(sshUser, sshHost, sshPort);
            session.setPassword(sshPassword);
            session.setConfig("StrictHostKeyChecking", "no");
            session.connect(3000);

            long timestampSeconds = System.currentTimeMillis() / 1000;
            String remoteCommand = "date -s @" + timestampSeconds + " && hwclock -w";

            ChannelExec channel = (ChannelExec) session.openChannel("exec");
            channel.setCommand(remoteCommand);
            channel.connect();

            while (!channel.isClosed()) {
                Thread.sleep(100);
            }

            int exitStatus = channel.getExitStatus();
            channel.disconnect();
            session.disconnect();
            return exitStatus == 0;

        } catch (Exception e) {
            Log.e("SSHTimeSync", "Error syncing time: " + e.getMessage(), e);
            return false;
        }
    }
}
