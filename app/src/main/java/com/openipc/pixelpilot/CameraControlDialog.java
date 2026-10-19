package com.openipc.pixelpilot;

import android.app.AlertDialog;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.text.InputType;
import android.util.Base64;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.Switch;
import android.widget.TextView;

import com.openipc.wfbngrtl8812.WfbNgLink;

import org.json.JSONObject;

import java.net.URLEncoder;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Iterator;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.function.Consumer;

/**
 * Drone -> Settings in APFPV mode: the camera's own controls. Reached either over the
 * USB adapter's APFPV link (a WebView cannot be used there - it runs on the phone's network
 * stack, which does not see the camera behind the adapter) or over the phone's own Wi-Fi
 * when that is the one connected to the camera. Talks to the two control planes of the
 * OpenIPC apfpv firmware directly:
 * <ul>
 *   <li>air_man_ap, TCP 12355, one text command per connection, no login: channel
 *       (20/40/80 MHz), video mode, region.</li>
 *   <li>the aalinkFPV web API, HTTP GET with the drone login: image options, aalink link
 *       parameters, flight-controller port, CPU clock, SSID, hostapd start channel,
 *       reboot. The same endpoints the camera's own web page uses.</li>
 * </ul>
 * All requests run one after another on a single background thread; the link carries one
 * TCP exchange at a time.
 */
final class CameraControlDialog {
    interface Listener {
        /** The camera's SSID was changed; it reboots and comes back with the new name. */
        void onSsidChanged(String ssid);
    }

    /** One TCP request/response with the camera; null when nothing came back. */
    interface Transport {
        /** @param halfClose end the sending side with the request, as {@code echo cmd | nc} does */
        String request(String ip, int port, String data, boolean halfClose, int timeoutMs);
    }

    /** Through the USB adapter's APFPV link (devourer + the userspace stack). */
    static Transport overAdapter(WfbNgLink link) {
        return (ip, port, data, halfClose, timeoutMs) -> link.cameraCommand(ip, port, data, timeoutMs, halfClose);
    }

    /**
     * Through the phone's own Wi-Fi. Sockets are bound to the Wi-Fi network: Android
     * otherwise prefers mobile data when the camera's network has no internet.
     */
    static Transport overWifi(Context context) {
        return (ip, port, data, halfClose, timeoutMs) -> {
            javax.net.SocketFactory factory = javax.net.SocketFactory.getDefault();
            android.net.ConnectivityManager cm = (android.net.ConnectivityManager)
                    context.getApplicationContext().getSystemService(Context.CONNECTIVITY_SERVICE);
            if (cm != null) {
                for (android.net.Network n : cm.getAllNetworks()) {
                    android.net.NetworkCapabilities caps = cm.getNetworkCapabilities(n);
                    if (caps != null && caps.hasTransport(android.net.NetworkCapabilities.TRANSPORT_WIFI)) {
                        factory = n.getSocketFactory();
                        break;
                    }
                }
            }
            java.io.ByteArrayOutputStream reply = new java.io.ByteArrayOutputStream();
            try (java.net.Socket s = factory.createSocket()) {
                s.connect(new java.net.InetSocketAddress(ip, port), Math.min(timeoutMs, 3000));
                s.setSoTimeout(timeoutMs);
                s.getOutputStream().write(data.getBytes(StandardCharsets.UTF_8));
                s.getOutputStream().flush();
                if (halfClose) s.shutdownOutput();
                java.io.InputStream in = s.getInputStream();
                byte[] buf = new byte[4096];
                int n;
                while ((n = in.read(buf)) > 0) {
                    reply.write(buf, 0, n);
                    // Like the adapter path: a short silence after the answer ends it,
                    // for servers that keep the connection open.
                    s.setSoTimeout(1500);
                }
            } catch (java.net.SocketTimeoutException e) {
                // Answer complete (or none at all, handled below).
            } catch (java.io.IOException e) {
                Log.w(TAG, "camera " + ip + ":" + port + " over Wi-Fi: " + e.getMessage());
            }
            return reply.size() > 0 ? new String(reply.toByteArray(), StandardCharsets.UTF_8) : null;
        };
    }

    private static final String TAG = "pixelpilot";
    private static final int AIR_MAN_PORT = 12355;
    private static final int HTTP_PORT = 80;
    private static final int TIMEOUT_MS = 15000;

    private static final String MAJ = "/cgi-bin/aalinkFPV-cli-maj.cgi";
    private static final String CONF = "/cgi-bin/aalinkFPV-conf.cgi";
    private static final String SERIAL = "/cgi-bin/aalinkFPV-serial.cgi";
    private static final String CPU = "/cgi-bin/aalinkFPV-cpu-oc.cgi";
    private static final String WLAN = "/cgi-bin/aalinkFPV-wlan.cgi";
    private static final String BOOT_CHANNEL = "/cgi-bin/aalinkFPV-channel-troubleshoot.cgi";
    private static final String MAIN_PAGE = "/cgi-bin/aalinkFPV.cgi";

    private static final String[] REGIONS = {"00", "US", "CA", "AU", "EU", "JP", "CN"};

    private final Context context;
    private final Transport transport;
    private final String via;
    private final String cameraIp;
    private final String auth;
    private final int appMaxWidthMhz;
    private final Listener listener;
    private final ExecutorService io = Executors.newSingleThreadExecutor();
    private final Handler main = new Handler(Looper.getMainLooper());

    private TextView status;
    private final List<String> channels = new ArrayList<>();
    private String currentChannel = "";
    private String wlanPass = "";

    // Rows that get filled once the current values are loaded.
    private RadioGroup widthGroup;
    private ChoiceRow channelRow, videoModeRow, regionRow, hqRow, sourceRow, serialRow, cpuRow, bootChannelRow;
    private ChoiceRow mcs20Row, mcs40Row, mcs80Row;
    private ToggleRow flipRow, overshootRow, focusRow;
    private SliderRow shiftRow, throughputRow, tempRow, txPowerRow, osdRow;
    private EditText ssidEdit;

    /** @param via shown in the title, e.g. "USB adapter" or "phone Wi-Fi" */
    CameraControlDialog(Context context, Transport transport, String via, String cameraIp, String user,
                        String password, int appMaxWidthMhz, Listener listener) {
        this.context = context;
        this.transport = transport;
        this.via = via;
        this.cameraIp = cameraIp;
        this.auth = Base64.encodeToString((user + ":" + password).getBytes(StandardCharsets.UTF_8),
                Base64.NO_WRAP);
        this.appMaxWidthMhz = appMaxWidthMhz;
        this.listener = listener;
    }

    void show() {
        ScrollView scroll = new ScrollView(context);
        LinearLayout root = new LinearLayout(context);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(40, 20, 40, 20);
        scroll.addView(root);

        status = new TextView(context);
        status.setText("Loading camera settings...");
        root.addView(status);

        buildLinkSection(root);
        buildVideoSection(root);
        buildAalinkSection(root);
        buildSystemSection(root);

        AlertDialog dialog = new AlertDialog.Builder(context)
                .setTitle("APFPV camera " + cameraIp + " (via " + via + ")")
                .setView(scroll)
                .setNegativeButton("Close", null)
                .create();
        dialog.setOnDismissListener(d -> io.shutdownNow());
        dialog.show();
        io.execute(this::loadAll);
    }

    // ------------------------------------------------------------------------
    // Sections
    // ------------------------------------------------------------------------

    private void buildLinkSection(LinearLayout root) {
        header(root, "Link");

        root.addView(label("Channel width"));
        widthGroup = new RadioGroup(context);
        widthGroup.setOrientation(RadioGroup.HORIZONTAL);
        for (int w : new int[]{20, 40, 80}) {
            RadioButton rb = new RadioButton(context);
            rb.setText(w + " MHz");
            rb.setId(View.generateViewId());
            rb.setTag(w);
            widthGroup.addView(rb);
        }
        widthGroup.setOnCheckedChangeListener((g, id) -> fillChannels(selectedWidth()));
        root.addView(widthGroup);

        channelRow = new ChoiceRow(root, "Channel", value -> {
            int width = widthOf(value);
            String note = width > appMaxWidthMhz
                    ? "\n\nThe app is set to at most " + appMaxWidthMhz + " MHz (APFPV settings), so it keeps "
                    + "receiving at that width."
                    : "";
            confirm("Switch the camera to channel " + value + "?",
                    "The camera announces the switch and the app follows it." + note,
                    () -> run("Channel", () -> {
                        String r = airman("set_ap_channel " + value);
                        currentChannel = airmanOr("get_current_ap_channel", currentChannel);
                        return r;
                    }));
        });

        regionRow = new ChoiceRow(root, "Region", value -> confirm(
                "Set the radio region to " + value + "?",
                "The region decides which channels the camera may use. Choosing one that does not "
                        + "match where you fly can break local radio regulations.",
                () -> run("Region", () -> {
                    String r = airman("set_region " + value);
                    loadChannels();
                    return r;
                })));
        regionRow.setItems(REGIONS, REGIONS);
    }

    private void buildVideoSection(LinearLayout root) {
        header(root, "Video");

        videoModeRow = new ChoiceRow(root, "Video mode", value -> confirm(
                "Switch the video mode to \"" + value + "\"?",
                "majestic restarts, the picture is gone for a few seconds. Modes that do not suit the "
                        + "sensor can leave the camera without video; modes that crop the sensor also "
                        + "add a crop to /etc/rc.local that applies on every boot.",
                () -> run("Video mode", () -> airman("set_simple_video_mode '" + value + "'"))));

        flipRow = new ToggleRow(root, "Flip image",
                on -> run("Flip image", () -> result(cgi(MAJ + "?action=set&value=" + on))));
        overshootRow = new ToggleRow(root, "Overshoot fix",
                on -> run("Overshoot fix", () -> result(cgi(MAJ + "?action=set_overshoot_fix&value=" + on))));
        hqRow = new ChoiceRow(root, "HQ i-frames",
                value -> run("HQ i-frames", () -> result(cgi(MAJ + "?action=set_hq_iframes&value=" + value))));
        hqRow.setItems(new String[]{"Off", "MQ", "MQ+", "HQ"}, new String[]{"off", "mq", "mqplus", "hq"});
        focusRow = new ToggleRow(root, "Focus mode (HQ centre)",
                on -> run("Focus mode", () -> result(cgi(CONF + "?module=focusmode&action=set&value=" + on))));
    }

    private void buildAalinkSection(LinearLayout root) {
        header(root, "aalink");

        String[] mcs = {"0", "1", "2", "3", "4", "5", "6", "7"};
        mcs20Row = mcsRow(root, "Max MCS 20 MHz", "max_mcs_20", mcs);
        mcs40Row = mcsRow(root, "Max MCS 40 MHz", "max_mcs_40", mcs);
        mcs80Row = mcsRow(root, "Max MCS 80 MHz", "max_mcs_80", mcs);

        shiftRow = new SliderRow(root, "Link resilience", -50, 50, 1, "%.0f",
                v -> run("Link resilience", () -> confSet("mcsshift", fmt("%.0f", v))));
        throughputRow = new SliderRow(root, "Maximum throughput (%)", 10, 100, 1, "%.0f",
                v -> run("Throughput", () -> confSet("throughput", fmt("%.0f", v))));
        sourceRow = new ChoiceRow(root, "LQ consideration",
                value -> run("LQ consideration", () -> confSet("mcssource", value)));
        sourceRow.setItems(new String[]{"Lowest", "Downlink only"}, new String[]{"lowest", "downlink"});
        tempRow = new SliderRow(root, "Temp throttle threshold (°C)", 70, 100, 1, "%.0f",
                v -> run("Temp threshold", () -> confSet("temp", fmt("%.0f", v))));
        txPowerRow = new SliderRow(root, "VTX power output", 0.2, 1.2, 0.05, "%.2f",
                v -> run("VTX power", () -> confSet("txPower", fmt("%.2f", v))));
        osdRow = new SliderRow(root, "OSD size", 0.2, 2.0, 0.1, "%.1f",
                v -> run("OSD size", () -> confSet("osdscale", fmt("%.1f", v))));
    }

    private void buildSystemSection(LinearLayout root) {
        header(root, "System");

        serialRow = new ChoiceRow(root, "Flight controller port", value -> run("Flight controller port",
                () -> result(cgi(SERIAL + "?action=set&value=" + enc(value)))));
        serialRow.setItems(new String[]{"None", "ttyS0", "ttyS1", "ttyS2", "ttyS3"},
                new String[]{"standalone", "/dev/ttyS0", "/dev/ttyS1", "/dev/ttyS2", "/dev/ttyS3"});

        cpuRow = new ChoiceRow(root, "CPU overclock", value -> run("CPU overclock",
                () -> result(cgi(CPU + "?action=set&value=" + value))));
        String[] cpu = {"none", "850", "900", "950", "1000", "1050", "1100", "1150", "1200"};
        String[] cpuLabels = new String[cpu.length];
        for (int i = 0; i < cpu.length; i++) cpuLabels[i] = i == 0 ? "None (800 MHz)" : cpu[i] + " MHz";
        cpuRow.setItems(cpuLabels, cpu);

        root.addView(label("SSID"));
        LinearLayout ssidLine = line();
        ssidEdit = new EditText(context);
        ssidEdit.setSingleLine(true);
        ssidEdit.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
        ssidLine.addView(ssidEdit);
        ssidLine.addView(button("Set", v -> {
            String ssid = ssidEdit.getText().toString();
            if (ssid.isEmpty() || ssid.length() > 32) {
                status("SSID: 1-32 characters");
                return;
            }
            confirm("Rename the camera's network to \"" + ssid + "\"?",
                    "The camera reboots and comes back under the new name; the app's APFPV settings are "
                            + "updated to match.",
                    () -> run("SSID", () -> {
                        // The CGI insists on a passphrase; the open-network adapter ignores it.
                        String pass = wlanPass.isEmpty() ? "12345678" : wlanPass;
                        String r = result(cgi(WLAN + "?action=set&ssid=" + enc(ssid) + "&pass=" + enc(pass)));
                        if (r != null && r.contains("ok")) main.post(() -> listener.onSsidChanged(ssid));
                        return r;
                    }));
        }));
        root.addView(ssidLine);

        bootChannelRow = new ChoiceRow(root, "hostapd start channel", value -> confirm(
                "Start the access point on channel " + value + "?",
                "Changes the channel hostapd starts on (before the switch to the stored channel) and "
                        + "reboots the camera.",
                () -> run("Start channel", () -> result(cgi(BOOT_CHANNEL + "?action=set&channel=" + value)))));
        LinearLayout bootLine = line();
        bootLine.addView(button("Restore 157", v -> confirm("Restore the default start channel 157?",
                "The camera reboots.",
                () -> run("Start channel", () -> result(cgi(BOOT_CHANNEL + "?action=restore"))))));
        root.addView(bootLine);

        LinearLayout rebootLine = line();
        rebootLine.addView(button("Reload values", v -> {
            status("Loading camera settings...");
            io.execute(this::loadAll);
        }));
        rebootLine.addView(button("Reboot camera", v -> confirm("Reboot the camera?",
                "The link drops and the app rejoins once the camera is back.",
                () -> run("Reboot", () -> result(cgi(MAIN_PAGE + "?action=reboot"))))));
        root.addView(rebootLine);
    }

    private ChoiceRow mcsRow(LinearLayout root, String title, String module, String[] values) {
        ChoiceRow row = new ChoiceRow(root, title, value -> run(title, () -> confSet(module, value)));
        row.setItems(values, values);
        return row;
    }

    // ------------------------------------------------------------------------
    // Loading
    // ------------------------------------------------------------------------

    /** Runs on the io thread: reads every value, then fills the rows on the main thread. */
    private void loadAll() {
        loadChannels();
        String modes = airman("get_all_video_modes");
        String mode = airman("get_current_video_mode");
        String region = airman("get_region");
        JSONObject flip = cgi(MAJ + "?action=get");
        JSONObject overshoot = cgi(MAJ + "?action=get_overshoot_fix");
        JSONObject hq = cgi(MAJ + "?action=get_hq_iframes");
        JSONObject focus = cgi(CONF + "?module=focusmode&action=get");
        JSONObject mcs20 = cgi(CONF + "?module=max_mcs_20&action=get");
        JSONObject mcs40 = cgi(CONF + "?module=max_mcs_40&action=get");
        JSONObject mcs80 = cgi(CONF + "?module=max_mcs_80&action=get");
        JSONObject shift = cgi(CONF + "?module=mcsshift&action=get");
        JSONObject throughput = cgi(CONF + "?module=throughput&action=get");
        JSONObject source = cgi(CONF + "?module=mcssource&action=get");
        JSONObject temp = cgi(CONF + "?module=temp&action=get");
        JSONObject txPower = cgi(CONF + "?module=txPower&action=get");
        JSONObject osd = cgi(CONF + "?module=osdscale&action=get");
        JSONObject serial = cgi(SERIAL + "?action=get");
        JSONObject cpu = cgi(CPU + "?action=get");
        JSONObject wlan = cgi(WLAN + "?action=get");

        main.post(() -> {
            fillChannels(widthOf(currentChannel));
            checkWidth(widthOf(currentChannel));
            if (modes != null) {
                String[] list = lines(modes);
                videoModeRow.setItems(list, list);
            }
            videoModeRow.select(mode);
            regionRow.select(region);
            if (flip != null) flipRow.set(flip.optBoolean("flipped"));
            if (overshoot != null) overshootRow.set(overshoot.optBoolean("enabled"));
            if (hq != null) hqRow.select(hq.optString("level"));
            if (focus != null) focusRow.set(focus.optBoolean("enabled"));
            mcs20Row.select(cardValue(mcs20));
            mcs40Row.select(cardValue(mcs40));
            mcs80Row.select(cardValue(mcs80));
            if (shift != null) shiftRow.set(shift.optDouble("mcsShift", 0));
            if (throughput != null) throughputRow.set(throughput.optDouble("throughput", 50));
            if (source != null) sourceRow.select(source.optString("mcsSource"));
            if (temp != null) tempRow.set(temp.optDouble("highTemp", 95));
            if (txPower != null) txPowerRow.set(txPower.optDouble("txPower", 1.0));
            if (osd != null) osdRow.set(osd.optDouble("osdScale", 1.0));
            if (serial != null) serialRow.select(serial.optString("current"));
            if (cpu != null) {
                String cur = cpu.optString("current");
                cpuRow.select("800".equals(cur) ? "none" : cur);
            }
            if (wlan != null) {
                ssidEdit.setText(wlan.optString("wlanssid"));
                wlanPass = wlan.optString("wlanpass");
            }
            boolean any = modes != null || flip != null;
            status(any ? "Camera settings loaded. Each \"Set\" applies one value right away."
                    : "The camera did not answer. Is the phone connected to it (APFPV via the USB adapter or the phone Wi-Fi)?");
        });
    }

    /** io thread. */
    private void loadChannels() {
        String list = airman("get_all_ap_channels");
        String cur = airman("get_current_ap_channel");
        synchronized (channels) {
            channels.clear();
            if (list != null) {
                for (String c : lines(list)) channels.add(c);
            }
        }
        if (cur != null) currentChannel = cur;
        main.post(() -> fillChannels(selectedWidth()));
    }

    /** Main thread: the channel list for one width, like the camera's own page sorts it. */
    private void fillChannels(int width) {
        List<String> values = new ArrayList<>();
        List<String> labels = new ArrayList<>();
        List<String> twenty = new ArrayList<>();
        synchronized (channels) {
            for (String c : channels) {
                int w = widthOf(c);
                if (w == 20) twenty.add(c);
                if (w != width) continue;
                values.add(c);
                labels.add(w == 20 ? c : c.replace("_", "-") + " (" + w + " MHz)");
            }
        }
        channelRow.setItems(labels.toArray(new String[0]), values.toArray(new String[0]));
        channelRow.select(currentChannel);
        String[] t = twenty.toArray(new String[0]);
        bootChannelRow.setItems(t, t);
        bootChannelRow.select("157");
    }

    private void checkWidth(int width) {
        for (int i = 0; i < widthGroup.getChildCount(); i++) {
            View v = widthGroup.getChildAt(i);
            if ((int) v.getTag() == width) {
                widthGroup.check(v.getId());
                return;
            }
        }
    }

    private int selectedWidth() {
        View v = widthGroup.findViewById(widthGroup.getCheckedRadioButtonId());
        return v != null ? (int) v.getTag() : 20;
    }

    /** "149" is 20 MHz, "149_153" 40 MHz, "149_161" 80 MHz (the camera page's rule). */
    private static int widthOf(String channel) {
        if (channel == null || !channel.contains("_")) return 20;
        String[] p = channel.split("_");
        try {
            return Integer.parseInt(p[1].trim()) - Integer.parseInt(p[0].trim()) <= 8 ? 40 : 80;
        } catch (RuntimeException e) {
            return 20;
        }
    }

    /** The max-MCS endpoints answer per adapter family ({"EU":5,"AU":5,...}); they are set together. */
    private static String cardValue(JSONObject o) {
        if (o == null) return null;
        if (o.has("EU")) return String.valueOf(o.optInt("EU"));
        Iterator<String> keys = o.keys();
        return keys.hasNext() ? String.valueOf(o.optInt(keys.next())) : null;
    }

    // ------------------------------------------------------------------------
    // Requests (io thread)
    // ------------------------------------------------------------------------

    private interface Request {
        String call();
    }

    /** Runs a change on the io thread and reports the camera's answer. */
    private void run(String what, Request request) {
        status(what + ": sending...");
        io.execute(() -> {
            String reply = request.call();
            String text = reply == null ? "no answer from the camera" : reply.trim().replace('\n', ' ');
            Log.i(TAG, "camera " + what + ": " + text);
            main.post(() -> status(what + ": " + text));
        });
    }

    /** One air_man_ap command; null when nothing came back. Retries while the link is busy. */
    private String airman(String command) {
        for (int attempt = 0; attempt < 3; attempt++) {
            String reply = transport.request(cameraIp, AIR_MAN_PORT, command + "\n", true, TIMEOUT_MS);
            if (reply != null) return reply.trim();
            if (!pause(400)) break;
        }
        return null;
    }

    private String airmanOr(String command, String fallback) {
        String r = airman(command);
        return r != null ? r : fallback;
    }

    /** GET on the camera's web API with the drone login; the JSON body, or null. */
    private JSONObject cgi(String pathAndQuery) {
        String request = "GET " + pathAndQuery + " HTTP/1.0\r\n"
                + "Host: " + cameraIp + "\r\n"
                + "Authorization: Basic " + auth + "\r\n"
                + "Connection: close\r\n\r\n";
        for (int attempt = 0; attempt < 3; attempt++) {
            String reply = transport.request(cameraIp, HTTP_PORT, request, false, TIMEOUT_MS);
            if (reply != null) {
                int nl = reply.indexOf('\n');
                String statusLine = nl > 0 ? reply.substring(0, nl).trim() : reply.trim();
                if (!statusLine.contains(" 200")) {
                    Log.w(TAG, "camera " + pathAndQuery + ": " + statusLine);
                    return null;
                }
                int brace = reply.indexOf('{', reply.indexOf("\r\n\r\n") + 1);
                if (brace < 0) return null;
                try {
                    return new JSONObject(reply.substring(brace).trim());
                } catch (Exception e) {
                    // Most likely a reply that was cut off on the way: ask again.
                    Log.w(TAG, "camera " + pathAndQuery + ": unreadable answer", e);
                    if (!pause(400)) break;
                    continue;
                }
            }
            if (!pause(400)) break;
        }
        return null;
    }

    private String confSet(String module, String value) {
        return result(cgi(CONF + "?module=" + module + "&action=set&value=" + enc(value)));
    }

    /** "ok" / the error text of a CGI answer. */
    private static String result(JSONObject o) {
        if (o == null) return null;
        if (o.has("error")) return "error: " + o.optString("error");
        return o.toString();
    }

    private static boolean pause(long ms) {
        try {
            Thread.sleep(ms);
            return true;
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return false;
        }
    }

    private static String enc(String s) {
        try {
            return URLEncoder.encode(s, "UTF-8");
        } catch (java.io.UnsupportedEncodingException e) {
            return s;
        }
    }

    private static String fmt(String format, double v) {
        return String.format(Locale.US, format, v);
    }

    private static String[] lines(String s) {
        List<String> out = new ArrayList<>();
        for (String l : s.split("\r?\n")) {
            if (!l.trim().isEmpty()) out.add(l.trim());
        }
        return out.toArray(new String[0]);
    }

    // ------------------------------------------------------------------------
    // UI helpers (main thread)
    // ------------------------------------------------------------------------

    private void status(String text) {
        if (status != null) status.setText(text);
    }

    private void confirm(String title, String message, Runnable onYes) {
        new AlertDialog.Builder(context)
                .setTitle(title)
                .setMessage(message)
                .setPositiveButton("Apply", (d, w) -> onYes.run())
                .setNegativeButton("Cancel", null)
                .show();
    }

    private void header(LinearLayout root, String text) {
        TextView tv = new TextView(context);
        tv.setText(text);
        tv.setTextSize(18);
        tv.setPadding(0, 30, 0, 6);
        root.addView(tv);
    }

    private TextView label(String text) {
        TextView tv = new TextView(context);
        tv.setText(text);
        tv.setPadding(0, 12, 0, 0);
        return tv;
    }

    private LinearLayout line() {
        LinearLayout l = new LinearLayout(context);
        l.setOrientation(LinearLayout.HORIZONTAL);
        l.setGravity(Gravity.CENTER_VERTICAL);
        return l;
    }

    private Button button(String text, View.OnClickListener onClick) {
        Button b = new Button(context);
        b.setText(text);
        b.setOnClickListener(onClick);
        return b;
    }

    /** Spinner + Set button; nothing is sent until Set is pressed. */
    private final class ChoiceRow {
        private final Spinner spinner;
        private String[] values = new String[0];

        ChoiceRow(LinearLayout root, String title, Consumer<String> onSet) {
            root.addView(label(title));
            LinearLayout l = line();
            spinner = new Spinner(context);
            spinner.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            l.addView(spinner);
            l.addView(button("Set", v -> {
                int i = spinner.getSelectedItemPosition();
                if (i >= 0 && i < values.length) onSet.accept(values[i]);
            }));
            root.addView(l);
        }

        void setItems(String[] labels, String[] values) {
            this.values = values;
            ArrayAdapter<String> adapter = new ArrayAdapter<>(context,
                    android.R.layout.simple_spinner_item, labels);
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            spinner.setAdapter(adapter);
        }

        void select(String value) {
            if (value == null) return;
            for (int i = 0; i < values.length; i++) {
                if (values[i].equalsIgnoreCase(value.trim())) {
                    spinner.setSelection(i);
                    return;
                }
            }
        }
    }

    /** Switch that applies on tap (not on programmatic set, so loading sends nothing). */
    private final class ToggleRow {
        private final Switch toggle;

        ToggleRow(LinearLayout root, String title, Consumer<Boolean> onChange) {
            toggle = new Switch(context);
            toggle.setText(title);
            toggle.setPadding(0, 12, 0, 0);
            toggle.setOnClickListener(v -> onChange.accept(toggle.isChecked()));
            root.addView(toggle);
        }

        void set(boolean on) {
            toggle.setChecked(on);
        }
    }

    /** SeekBar over [min, max] in steps, value shown beside it, sent with Set. */
    private final class SliderRow {
        private final SeekBar seek;
        private final TextView value;
        private final double min, step;
        private final String format;

        SliderRow(LinearLayout root, String title, double min, double max, double step, String format,
                  Consumer<Double> onSet) {
            this.min = min;
            this.step = step;
            this.format = format;
            root.addView(label(title));
            LinearLayout l = line();
            seek = new SeekBar(context);
            seek.setMax((int) Math.round((max - min) / step));
            seek.setLayoutParams(new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1));
            value = new TextView(context);
            value.setMinWidth(120);
            value.setGravity(Gravity.END);
            seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
                @Override
                public void onProgressChanged(SeekBar s, int p, boolean fromUser) {
                    value.setText(fmt(SliderRow.this.format, current()));
                }

                @Override
                public void onStartTrackingTouch(SeekBar s) {
                }

                @Override
                public void onStopTrackingTouch(SeekBar s) {
                }
            });
            l.addView(seek);
            l.addView(value);
            l.addView(button("Set", v -> onSet.accept(current())));
            root.addView(l);
            set(min);
        }

        double current() {
            return min + seek.getProgress() * step;
        }

        void set(double v) {
            seek.setProgress((int) Math.round((v - min) / step));
            value.setText(fmt(format, current()));
        }
    }
}
