package com.openipc.pixelpilot;

import android.annotation.SuppressLint;
import android.app.Dialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.UriPermission;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.net.VpnService;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.text.format.Formatter;
import android.util.Base64;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SubMenu;
import android.view.View;
import android.view.WindowManager;
import android.webkit.HttpAuthHandler;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.PopupMenu;
import android.widget.SeekBar;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.constraintlayout.widget.ConstraintLayout;
import androidx.constraintlayout.widget.ConstraintSet;
import androidx.core.content.FileProvider;
import androidx.documentfile.provider.DocumentFile;

import com.github.mikephil.charting.charts.PieChart;
import com.github.mikephil.charting.data.PieData;
import com.github.mikephil.charting.data.PieDataSet;
import com.github.mikephil.charting.data.PieEntry;
import com.github.mikephil.charting.formatter.PercentFormatter;
import com.openipc.mavlink.MavlinkData;
import com.openipc.mavlink.MavlinkNative;
import com.openipc.mavlink.MavlinkUpdate;
import com.openipc.pixelpilot.databinding.ActivityVideoBinding;
import com.openipc.pixelpilot.osd.OSDElement;
import com.openipc.pixelpilot.osd.OSDManager;
import com.openipc.videonative.DecodingInfo;
import com.openipc.videonative.IVideoParamsChanged;
import com.openipc.videonative.VideoPlayer;
import com.openipc.wfbngrtl8812.WfbNGStats;
import com.openipc.wfbngrtl8812.WfbNGStatsChanged;
import com.openipc.wfbngrtl8812.WfbNgLink;

import java.io.BufferedReader;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.text.SimpleDateFormat;
import java.time.LocalDateTime;
import java.time.format.DateTimeFormatter;
import java.util.ArrayList;
import java.util.Date;
import java.util.Locale;
import java.util.Timer;
import java.util.TimerTask;

// Most basic implementation of an activity that uses VideoNative to stream a video
// Into an Android Surface View
public class VideoActivity extends AppCompatActivity implements IVideoParamsChanged,
        WfbNGStatsChanged, MavlinkUpdate, SettingsChanged {
    private static final String TAG = "pixelpilot";
    private static final int PICK_KEY_REQUEST_CODE = 1;
    private static final int PICK_DVR_REQUEST_CODE = 2;
    private static WifiManager wifiManager;
    final Handler handler = new Handler(Looper.getMainLooper());
    final Runnable runnable = new Runnable() {
        public void run() {
            MavlinkNative.nativeCallBack(VideoActivity.this);
            handler.postDelayed(this, 100);
        }
    };
    protected DecodingInfo mDecodingInfo;
    int lastVideoW = 0, lastVideoH = 0, lastCodec = 1;
    WfbLinkManager wfbLinkManager;
    BroadcastReceiver batteryReceiver;
    VideoPlayer videoPlayer;
    private ActivityVideoBinding binding;
    private OSDManager osdManager;
    private ParcelFileDescriptor dvrFd = null;
    private Timer dvrIconTimer = null;
    private Timer recordTimer = null;
    private int seconds = 0;
    private boolean isVRMode = false;
    private ConstraintLayout constraintLayout;
    private ConstraintSet constraintSet;
    private WfbNgLink wfbLink;

    public boolean getVRSetting() {
        return getSharedPreferences("general", Context.MODE_PRIVATE).getBoolean("vr-mode", false);
    }

    public void setVRSetting(boolean v) {
        SharedPreferences prefs = getSharedPreferences("general", Context.MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putBoolean("vr-mode", v);
        editor.commit();
    }

    public static int getChannel(Context context) {
        return context.getSharedPreferences("general",
                Context.MODE_PRIVATE).getInt("wifi-channel", 161);
    }

    public static int getBandwidth(Context context) {
        return context.getSharedPreferences("general",
                Context.MODE_PRIVATE).getInt("bandwidth", 20);
    }

    public static String wirelessInfo() {
        int address = wifiManager.getConnectionInfo().getIpAddress();
        return (address == 0) ? null : Formatter.formatIpAddress(address);
    }

    static String paddedDigits(int val, int len) {
        StringBuilder sb = new StringBuilder(String.format("%d", val));
        while (sb.length() < len) {
            sb.append('\t');
        }
        return sb.toString();
    }

    public static String bytesToHex(byte[] bytes) {
        StringBuilder hexString = new StringBuilder();
        for (byte b : bytes) {
            String hex = Integer.toHexString(0xFF & b);
            if (hex.length() == 1) {
                // Append a leading zero for single digit hex values
                hexString.append('0');
            }
            hexString.append(hex);
        }
        return hexString.toString();
    }

    private void resetApp() {
        // Restart the app
        Intent intent = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (intent != null) {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(intent);
            finish();
            System.exit(0); // Ensure the app is fully restarted
        }
    }

    private boolean hasUriPermission(Uri uri) {
        for (UriPermission perm : getContentResolver().getPersistedUriPermissions()) {
            if (perm.getUri().equals(uri) && perm.isWritePermission()) {
                return true;
            }
        }
        return false;
    }

    private void resetFolderPermissions() {
        // Retrieve the stored DVR folder URI
        SharedPreferences prefs = getSharedPreferences("general", Context.MODE_PRIVATE);
        String dvrFolderUriString = prefs.getString("dvr_folder_", null);
        if (dvrFolderUriString == null) {
            Toast.makeText(this, "No folder permissions to reset.", Toast.LENGTH_SHORT).show();
            return;
        }

        Uri dvrUri = Uri.parse(dvrFolderUriString);

        // Revoke persisted URI permissions
        for (UriPermission perm : getContentResolver().getPersistedUriPermissions()) {
            if (perm.getUri().equals(dvrUri)) {
                getContentResolver().releasePersistableUriPermission(perm.getUri(), Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                Log.d(TAG, "Released URI permission for: " + perm.getUri());
            }
        }

        // Clear the stored URI from SharedPreferences
        SharedPreferences.Editor editor = prefs.edit();
        editor.remove("dvr_folder_");
        editor.apply();

        // Stop any ongoing DVR recording
        if (dvrFd != null) {
            stopDvr();
        }

        // Update the record button icon to default
        binding.imgBtnRecord.setImageResource(R.drawable.record);

        // Reset any related UI elements
        binding.txtRecordLabel.setVisibility(View.GONE);
        binding.imgRecIndicator.setVisibility(View.INVISIBLE);

        // Inform the user
        Toast.makeText(this, "Folder permissions have been reset.", Toast.LENGTH_LONG).show();

        // Optionally, prompt the user to select a new folder immediately
        // Uncomment the following lines if you want to prompt immediately
        /*
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        intent.addCategory(Intent.CATEGORY_DEFAULT);
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                        Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
        startActivityForResult(intent, PICK_DVR_REQUEST_CODE);
        */
    }

    // Lifecycle - onCreate

    @SuppressLint("ClickableViewAccessibility")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.d(TAG, "lifecycle onCreate");
        super.onCreate(savedInstanceState);

        // UI Setup
        initializeUI();

        // WFB-NG Setup
        initializeWfbNg();

        // Video Player(s) Setup
        initializeVideoPlayers();

        // VR-specific SeekBars (only if VR mode)
        setupVRSeekBarsIfNeeded();

        // OSD Manager Setup
        setupOSDManager();

        // PieChart Setup
        setupPieChart();

        // Button Handlers
        setupButtonHandlers();

        // Mavlink Setup
        setupMavlink();

        // Battery Receiver
        setupBatteryReceiver();

        // wfbNg VPN Service
        startVpnService();
    }

    // ----------------------------------------------------------------------------
    // UI SETUP
    // ----------------------------------------------------------------------------

    /**
     * Initializes basic UI components, including window flags and layout binding.
     */
    private void initializeUI() {
        binding = ActivityVideoBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());

        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_HIDE_NAVIGATION);

        wifiManager = (WifiManager) getSystemService(WIFI_SERVICE);
    }

    // ----------------------------------------------------------------------------
    // WFB-NG SETUP
    // ----------------------------------------------------------------------------

    /**
     * Initializes WFB-NG related logic such as setting default gs.key and linking
     * to WFB-NG stats changes.
     */
    private void initializeWfbNg() {
        setDefaultGsKey();
        copyGSKey();
        wfbLink = new WfbNgLink(this);
        wfbLink.SetWfbNGStatsChanged(this);
        wfbLinkManager = new WfbLinkManager(this, binding, wfbLink);
    }

    // ----------------------------------------------------------------------------
    // VIDEO PLAYER SETUP
    // ----------------------------------------------------------------------------

    /**
     * Initializes VideoPlayer and configures surfaces for VR or standard mode.
     */
    private void initializeVideoPlayers() {
        videoPlayer = new VideoPlayer(this);
        videoPlayer.setIVideoParamsChanged(this);

        isVRMode = getVRSetting();

        if (isVRMode) {
            setupVRVideoPlayers();
        } else {
            setupStandardVideoPlayer();
        }
    }

    /**
     * Configures the UI for VR mode by attaching callbacks to the left and right SurfaceViews.
     */
    private void setupVRVideoPlayers() {
        binding.mainVideo.setVisibility(View.GONE);
        binding.surfaceViewLeft.getHolder().addCallback(videoPlayer.configure1(0));
        binding.surfaceViewRight.getHolder().addCallback(videoPlayer.configure1(1));
    }

    /**
     * Configures the UI for standard, single-surface video playback.
     */
    private void setupStandardVideoPlayer() {
        binding.surfaceViewRight.setVisibility(View.GONE);
        binding.surfaceViewLeft.setVisibility(View.GONE);
        binding.mainVideo.getHolder().addCallback(videoPlayer.configure1(0));
    }

    // ----------------------------------------------------------------------------
    // VR SEEK BARS (only in VR mode)
    // ----------------------------------------------------------------------------

    /**
     * Initializes and configures SeekBars for VR mode to adjust the margin and size of surfaces.
     * If not in VR mode, this method does nothing.
     */
    private void setupVRSeekBarsIfNeeded() {
        if (!isVRMode) return;

        constraintLayout = binding.frameLayout;
        constraintSet = new ConstraintSet();
        constraintSet.clone(constraintLayout);

        configureVRSeekBars();
        configureVRSeekBarVisibility();
        configureVRSeekBarListeners();
    }

    /**
     * Configures both the margin (binding.seekBar) and distance (binding.distanceSeekBar) SeekBars.
     */
    private void configureVRSeekBars() {
        // Rotate the first seekBar 180 degrees
        binding.seekBar.setRotation(180);

        // Retrieve saved progress for both seekBars
        SharedPreferences sharedPreferences = getSharedPreferences("SeekBarPrefs", MODE_PRIVATE);
        SharedPreferences sharedPreferencesd = getSharedPreferences("SeekBarPrefsD", MODE_PRIVATE);

        int savedProgress = sharedPreferences.getInt("seekBarProgress", 1);
        int savedDistanceProgress = sharedPreferencesd.getInt("distanceSeekBarProgress", 1);

        // Apply saved progress values
        binding.seekBar.setProgress(savedProgress);
        binding.distanceSeekBar.setProgress(savedDistanceProgress);

        // Make them visible initially
        binding.seekBar.setVisibility(View.VISIBLE);
        binding.distanceSeekBar.setVisibility(View.VISIBLE);

        // Apply initial constraints
        applyVRMargins(savedProgress);
        applyVRDistance(savedDistanceProgress);
    }

    /**
     * Manages hiding and showing the SeekBars after some delay or upon user touch.
     */
    private void configureVRSeekBarVisibility() {
        // Hide SeekBars after 3 seconds
        handler.postDelayed(() -> {
            binding.seekBar.setVisibility(View.GONE);
            binding.distanceSeekBar.setVisibility(View.GONE);
            updateViewRatio(R.id.surfaceViewLeft, lastVideoW, lastVideoH);
            updateViewRatio(R.id.surfaceViewRight, lastVideoW, lastVideoH);
        }, 3000);

        // Show SeekBars when the layout is touched
        binding.frameLayout.setOnTouchListener((v, event) -> {
            if (event.getAction() == MotionEvent.ACTION_DOWN) {
                binding.seekBar.setVisibility(View.VISIBLE);
                binding.distanceSeekBar.setVisibility(View.VISIBLE);
                handler.postDelayed(() -> {
                    binding.seekBar.setVisibility(View.GONE);
                    binding.distanceSeekBar.setVisibility(View.GONE);
                    updateViewRatio(R.id.surfaceViewLeft, lastVideoW, lastVideoH);
                    updateViewRatio(R.id.surfaceViewRight, lastVideoW, lastVideoH);
                }, 3000);
            }
            return false;
        });
    }

    /**
     * Sets listeners on the SeekBars to adjust margins and distances in real time.
     */
    private void configureVRSeekBarListeners() {
        binding.seekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                applyVRMargins(progress);
                saveSeekBarValue("SeekBarPrefs", "seekBarProgress", progress);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });

        binding.distanceSeekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar distanceSeekBar, int progress, boolean fromUser) {
                applyVRDistance(progress);
                saveSeekBarValue("SeekBarPrefsD", "distanceSeekBarProgress", progress);
            }

            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {
            }

            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {
            }
        });
    }

    /**
     * Adjusts margins for left/right SurfaceViews based on progress.
     */
    private void applyVRMargins(int progress) {
        int margin = progress * 20; // Adjust multiplier as needed
        constraintSet.setMargin(R.id.surfaceViewLeft, ConstraintSet.START, margin);
        constraintSet.setMargin(R.id.surfaceViewRight, ConstraintSet.END, margin);
        constraintSet.applyTo(constraintLayout);
    }

    /**
     * Adjusts size for left/right SurfaceViews based on progress.
     */
    private void applyVRDistance(int progress) {
        int size = progress * 20; // Adjust multiplier as needed
        constraintSet.setMargin(R.id.surfaceViewLeft, ConstraintSet.END, size);
        constraintSet.setMargin(R.id.surfaceViewRight, ConstraintSet.START, size);
        constraintSet.applyTo(constraintLayout);
    }

    /**
     * Saves the SeekBar progress value to SharedPreferences.
     */
    private void saveSeekBarValue(String prefsName, String key, int progress) {
        SharedPreferences sp = getSharedPreferences(prefsName, MODE_PRIVATE);
        SharedPreferences.Editor editor = sp.edit();
        editor.putInt(key, progress);
        editor.apply();
    }

    // ----------------------------------------------------------------------------
    // OSD MANAGER
    // ----------------------------------------------------------------------------

    /**
     * Sets up the On-Screen Display (OSD) manager for telemetry or other overlays.
     */
    private void setupOSDManager() {
        osdManager = new OSDManager(this, binding);
        osdManager.setUp();
    }

    // ----------------------------------------------------------------------------
    // PIECHART SETUP
    // ----------------------------------------------------------------------------

    /**
     * Initializes and configures the PieChart to show link statistics (initially empty).
     */
    private void setupPieChart() {
        PieChart chart = binding.pcLinkStat;
        chart.getLegend().setEnabled(false);
        chart.getDescription().setEnabled(false);
        chart.setDrawHoleEnabled(true);
        chart.setHoleColor(Color.WHITE);
        chart.setTransparentCircleColor(Color.WHITE);
        chart.setTransparentCircleAlpha(110);
        chart.setHoleRadius(58f);
        chart.setTransparentCircleRadius(61f);
        chart.setHighlightPerTapEnabled(false);
        chart.setRotationEnabled(false);
        chart.setClickable(false);
        chart.setTouchEnabled(false);

        PieData emptyData = new PieData(new PieDataSet(new ArrayList<>(), ""));
        chart.setData(emptyData);
    }

    // ----------------------------------------------------------------------------
    // BUTTON HANDLERS
    // ----------------------------------------------------------------------------

    /**
     * Sets up the main button click listeners: Record and Settings.
     */
    private void setupButtonHandlers() {
        binding.imgBtnRecord.setOnClickListener(item -> startStopDvr());
        binding.btnSettings.setOnClickListener(this::showSettingsMenu);
    }

    /**
     * Shows the main settings popup menu and configures its items.
     */
    private void showSettingsMenu(View anchor) {
        PopupMenu popup = new PopupMenu(this, anchor);

        // VR submenu
        setupVRSubMenu(popup);

        // Channel submenu
        setupChannelSubMenu(popup);

        // Bandwidth submenu
        setupBandwidthSubMenu(popup);

        // OSD submenu
        setupOSDSubMenu(popup);

        // WFB submenu
        setupWFBSubMenu(popup);

        // Adaptive link submenu
        setupAdaptiveLinkSubMenu(popup);

        // Recording submenu
        setupRecordingSubMenu(popup);

        // Drone submenu
        setupDroneSubMenu(popup);

        // Help submenu
        setupHelpSubMenu(popup);

        popup.show();
    }

    /**
     * Submenu that toggles VR mode.
     */
    private void setupVRSubMenu(PopupMenu popup) {
        SubMenu vrMenu = popup.getMenu().addSubMenu("VR mode");
        MenuItem vrItem = vrMenu.add(getVRSetting() ? "On" : "Off");
        vrItem.setOnMenuItemClickListener(item -> {
            isVRMode = !getVRSetting();
            setVRSetting(isVRMode);
            vrItem.setTitle(isVRMode ? "Off" : "On");
            item.setShowAsAction(MenuItem.SHOW_AS_ACTION_COLLAPSE_ACTION_VIEW);
            item.setActionView(new View(this));
            resetApp();
            return false;
        });
    }

    /**
     * Submenu that lists available channels and allows the user to select one.
     */
    private void setupChannelSubMenu(PopupMenu popup) {
        SubMenu chnMenu = popup.getMenu().addSubMenu("Channel");
        int channelPref = getChannel(this);
        chnMenu.setHeaderTitle("Current: " + channelPref);

        String[] channels = getResources().getStringArray(R.array.channels);
        for (String chnStr : channels) {
            chnMenu.add(chnStr).setOnMenuItemClickListener(item -> {
                onChannelSettingChanged(Integer.parseInt(chnStr));
                return true;
            });
        }
    }

    /**
     * Submenu that allows the user to select 20 or 40 MHz bandwidth.
     */
    private void setupBandwidthSubMenu(PopupMenu popup) {
        SubMenu bwMenu = popup.getMenu().addSubMenu("Bandwidth");
        int bandwidthPref = getBandwidth(this);
        bwMenu.setHeaderTitle("Current: " + bandwidthPref);

        String[] bws = getResources().getStringArray(R.array.bandwidths);
        for (String bwStr : bws) {
            bwMenu.add(bwStr).setOnMenuItemClickListener(item -> {
                onBandwidthSettingChanged(Integer.parseInt(bwStr));
                return true;
            });
        }
    }

    /**
     * Submenu handling OSD toggles and locks.
     */
    private void setupOSDSubMenu(PopupMenu popup) {
        SubMenu osd = popup.getMenu().addSubMenu("OSD");
        MenuItem lock = osd.add(osdManager.getTitle());
        lock.setOnMenuItemClickListener(item -> {
            osdManager.lockOSD(!osdManager.isOSDLocked());
            lock.setTitle(osdManager.getTitle());
            item.setShowAsAction(MenuItem.SHOW_AS_ACTION_COLLAPSE_ACTION_VIEW);
            item.setActionView(new View(this));
            return false;
        });

        for (OSDElement element : osdManager.listOSDItems) {
            MenuItem itm = osd.add(element.name);
            itm.setCheckable(true);
            itm.setChecked(osdManager.isElementEnabled(element));
            itm.setOnMenuItemClickListener(menuItem -> {
                menuItem.setChecked(!menuItem.isChecked());
                osdManager.onOSDItemCheckChanged(element, menuItem.isChecked());
                menuItem.setShowAsAction(MenuItem.SHOW_AS_ACTION_COLLAPSE_ACTION_VIEW);
                menuItem.setActionView(new View(this));
                return false;
            });
        }
    }

    /**
     * Submenu handling WFB-NG logic (e.g. selecting gs.key from storage).
     */
    private void setupWFBSubMenu(PopupMenu popup) {
        SubMenu wfb = popup.getMenu().addSubMenu("WFB-NG key");
        MenuItem keyBtn = wfb.add("gs.key");
        keyBtn.setOnMenuItemClickListener(item -> {
            Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("*/*");
            startActivityForResult(intent, PICK_KEY_REQUEST_CODE);
            return true;
        });
    }

    /**
     * Submenu for Adaptive link functionality.
     * It creates two options:
     * - "Enable": toggles the adaptive link quality thread
     * - "Power": a submenu that lets the user choose the TX power (1, 10, 20, 30, 40)
     */
    private void setupAdaptiveLinkSubMenu(PopupMenu popup) {
        SubMenu adaptiveMenu = popup.getMenu().addSubMenu("Adaptive link");

        SharedPreferences prefs = getSharedPreferences("general", MODE_PRIVATE);
        boolean adaptiveEnabled = prefs.getBoolean("adaptive_link_enabled", true);
        int adaptiveTxPower = prefs.getInt("adaptive_tx_power", 30);
         wfbLink.nativeSetAdaptiveLinkEnabled(adaptiveEnabled);
         wfbLink.nativeSetTxPower(adaptiveTxPower);

        // Adaptive link Enable option
        MenuItem adaptiveEnable = adaptiveMenu.add("Enable");
        adaptiveEnable.setCheckable(true);
        adaptiveEnable.setChecked(adaptiveEnabled);
        adaptiveEnable.setOnMenuItemClickListener(item -> {
            boolean newState = !item.isChecked();
            item.setChecked(newState);
            SharedPreferences.Editor editor = getSharedPreferences("general", MODE_PRIVATE).edit();
            editor.putBoolean("adaptive_link_enabled", newState);
            editor.apply();
            // Call instance method on the WfbNgLink instance via the wfbLinkManager.
            wfbLink.nativeSetAdaptiveLinkEnabled(newState);
            return true;
        });

        // Adaptive link Power submenu
        SubMenu powerSubMenu = adaptiveMenu.addSubMenu("Power");
        int[] txOptions = {1, 10, 20, 30, 40};
        for (int power : txOptions) {
            MenuItem powerItem = powerSubMenu.add(String.valueOf(power));
            powerItem.setCheckable(true);
            if (power == adaptiveTxPower) {
                powerItem.setChecked(true);
            }
            powerItem.setOnMenuItemClickListener(item -> {
                // Uncheck all items in the submenu
                for (int i = 0; i < powerSubMenu.size(); i++) {
                    powerSubMenu.getItem(i).setChecked(false);
                }
                item.setChecked(true);
                SharedPreferences.Editor editor = getSharedPreferences("general", MODE_PRIVATE).edit();
                editor.putInt("adaptive_tx_power", power);
                editor.apply();
                // Call instance method on the WfbNgLink instance via the wfbLinkManager.
                wfbLink.nativeSetTxPower(power);
                return true;
            });
        }
    }

    /**
     * Submenu for recording options, including start/stop DVR and toggling fMP4.
     */
    private void setupRecordingSubMenu(PopupMenu popup) {
        SubMenu recording = popup.getMenu().addSubMenu("Recording");

        MenuItem dvrBtn = recording.add(dvrFd == null ? "Start" : "Stop");
        dvrBtn.setOnMenuItemClickListener(item -> {
            startStopDvr();
            return true;
        });

        MenuItem fmp4 = recording.add("fMP4");
        fmp4.setCheckable(true);
        fmp4.setChecked(getDvrMP4());
        fmp4.setOnMenuItemClickListener(item -> {
            boolean enabled = getDvrMP4();
            item.setChecked(!enabled);
            setDvrMP4(!enabled);
            item.setShowAsAction(MenuItem.SHOW_AS_ACTION_COLLAPSE_ACTION_VIEW);
            item.setActionView(new View(this));
            return false;
        });

        MenuItem resetPermissions = recording.add("Reset DVR folder");
        resetPermissions.setOnMenuItemClickListener(item -> {
            resetFolderPermissions();
            return true;
        });
    }

    /**
     * Submenu for drone settings.
     */
    private void setupDroneSubMenu(PopupMenu popup) {
        SubMenu drone = popup.getMenu().addSubMenu("Drone");
        MenuItem settings = drone.add("Settings");
        settings.setOnMenuItemClickListener(item -> {
            startBrowser();
            return true;
        });
    }

    /**
     * Submenu for help items, such as sending logs.
     */
    private void setupHelpSubMenu(PopupMenu popup) {
        SubMenu help = popup.getMenu().addSubMenu("Help");
        MenuItem logs = help.add("Send Logs");

        // Increase logcat buffer to 10MB if possible
        try {
            Runtime.getRuntime().exec("logcat -G 10M");
        } catch (IOException e) {
            Log.e(TAG, "ShareLog: ", e);
        }

        logs.setOnMenuItemClickListener(item -> {
            shareLogs();
            return true;
        });
    }

    // ----------------------------------------------------------------------------
    // MAVLINK SETUP
    // ----------------------------------------------------------------------------

    /**
     * Starts the native Mavlink service and posts an initial Runnable to the Handler.
     */
    private void setupMavlink() {
        MavlinkNative.nativeStart(this);
        handler.post(runnable);
    }

    // ----------------------------------------------------------------------------
    // BATTERY RECEIVER
    // ----------------------------------------------------------------------------

    /**
     * Registers a receiver that listens for battery status changes and updates the UI accordingly.
     */
    private void setupBatteryReceiver() {
        batteryReceiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent batteryStatus) {
                updateBatteryStatus(batteryStatus);
            }
        };
    }

    /**
     * Updates the battery icon and percentage based on the current battery state.
     */
    private void updateBatteryStatus(Intent batteryStatus) {
        int status = batteryStatus.getIntExtra(BatteryManager.EXTRA_STATUS, -1);
        boolean isCharging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
                status == BatteryManager.BATTERY_STATUS_FULL;

        int level = batteryStatus.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        int scale = batteryStatus.getIntExtra(BatteryManager.EXTRA_SCALE, -1);
        float batteryPct = level * 100 / (float) scale;
        binding.tvGSBattery.setText((int) batteryPct + "%");

        int icon;
        if (isCharging) {
            icon = R.drawable.baseline_battery_charging_full_24;
        } else {
            // Adjust these thresholds as needed
            if (batteryPct <= 0) {
                icon = R.drawable.baseline_battery_0_bar_24;
            } else if (batteryPct <= (1f / 7f) * 100) {
                icon = R.drawable.baseline_battery_1_bar_24;
            } else if (batteryPct <= (2f / 7f) * 100) {
                icon = R.drawable.baseline_battery_2_bar_24;
            } else if (batteryPct <= (3f / 7f) * 100) {
                icon = R.drawable.baseline_battery_3_bar_24;
            } else if (batteryPct <= (4f / 7f) * 100) {
                icon = R.drawable.baseline_battery_4_bar_24;
            } else if (batteryPct <= (5f / 7f) * 100) {
                icon = R.drawable.baseline_battery_5_bar_24;
            } else if (batteryPct <= (6f / 7f) * 100) {
                icon = R.drawable.baseline_battery_6_bar_24;
            } else {
                icon = R.drawable.baseline_battery_full_24;
            }
        }
        binding.imgGSBattery.setImageResource(icon);
    }

    // ----------------------------------------------------------------------------
    // LOG SHARING
    // ----------------------------------------------------------------------------

    /**
     * Shares the device logs by writing them to a file and prompting the user to choose a share target.
     */
    private void shareLogs() {
        try {
            Process process = Runtime.getRuntime().exec("logcat -d");
            BufferedReader bufferedReader = new BufferedReader(new InputStreamReader(process.getInputStream()));

            String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss", Locale.getDefault()).format(new Date());
            File logFile = new File(getExternalFilesDir(null), "pixelpilot_log_" + timeStamp + ".txt");
            FileWriter fileWriter = new FileWriter(logFile);

            // Fetch app version info
            String versionName = "";
            long versionCode = 0;
            try {
                PackageInfo packageInfo = getPackageManager().getPackageInfo(getPackageName(), 0);
                versionName = packageInfo.versionName;
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
                    versionCode = packageInfo.getLongVersionCode();
                } else {
                    versionCode = packageInfo.versionCode;
                }
            } catch (PackageManager.NameNotFoundException ignored) {
            }

            // Write device/app info
            fileWriter.append("Device Model: ").append(Build.MODEL).append("\n")
                    .append("Manufacturer: ").append(Build.MANUFACTURER).append("\n")
                    .append("OS Version: ").append(Build.VERSION.RELEASE).append("\n")
                    .append("SDK Version: ").append(String.valueOf(Build.VERSION.SDK_INT)).append("\n")
                    .append("App Version Name: ").append(versionName).append("\n")
                    .append("App Version Code: ").append(String.valueOf(versionCode)).append("\n");

            // Write actual logs
            String line;
            while ((line = bufferedReader.readLine()) != null) {
                fileWriter.append(line).append("\n");
            }
            fileWriter.flush();
            fileWriter.close();

            // Share the log file
            Intent sendIntent = new Intent();
            sendIntent.setAction(Intent.ACTION_SEND);
            Uri fileUri = FileProvider.getUriForFile(this, getPackageName() + ".provider", logFile);
            sendIntent.putExtra(Intent.EXTRA_STREAM, fileUri);
            sendIntent.setType("text/plain");
            sendIntent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            Intent shareIntent = Intent.createChooser(sendIntent, null);
            startActivity(shareIntent);

        } catch (IOException e) {
            Log.e(TAG, "ShareLog: ", e);
        }
    }

    // ----------------------------------------------------------------------------
    // VPN SERVICE
    // ----------------------------------------------------------------------------
    private void startVpnService() {
        int VPN_REQUEST_CODE = 100;

        Intent intent = VpnService.prepare(this);
        if (intent != null) {
            startActivityForResult(intent, VPN_REQUEST_CODE);
        } else {
            Intent serviceIntent = new Intent(this, WfbNgVpnService.class);
            startService(serviceIntent);
        }

    }

    private Uri openDvrFile() {
        String dvrFolder = getSharedPreferences("general",
                Context.MODE_PRIVATE).getString("dvr_folder_", "");
        if (dvrFolder.isEmpty()) {
            Log.e(TAG, "dvrFolder is empty");
            return null;
        }
        Uri uri = Uri.parse(dvrFolder);
        DocumentFile pickedDir = DocumentFile.fromTreeUri(this, uri);
        if (pickedDir != null && pickedDir.canWrite()) {
            LocalDateTime now = LocalDateTime.now();
            DateTimeFormatter formatter = DateTimeFormatter.ofPattern("yyyyMMdd-HHmm");
            // Format the current date and time
            String formattedNow = now.format(formatter);
            String filename = "pixelpilot_" + formattedNow + ".mp4";
            DocumentFile newFile = pickedDir.createFile("video/mp4", filename);
            Toast.makeText(this, "Recording to " + filename, Toast.LENGTH_SHORT).show();
            if (newFile == null)
                Log.e(TAG, "dvr newFile null");
            return newFile != null ? newFile.getUri() : null;
        }
        return null;
    }

    private void startStopDvr() {
        if (dvrFd == null) {
            Uri dvrUri = openDvrFile();
            if (dvrUri != null) {
                startDvr(dvrUri);
            } else {
                wfbLinkManager.stopAdapters();
                videoPlayer.stop();
                videoPlayer.stopAudio();

                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                intent.addCategory(Intent.CATEGORY_DEFAULT);
                intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION |
                        Intent.FLAG_GRANT_WRITE_URI_PERMISSION |
                        Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION);
                startActivityForResult(intent, PICK_DVR_REQUEST_CODE);
            }
        } else {
            stopDvr();
        }
    }

    private void startDvr(Uri dvrUri) {
        if (dvrFd != null) {
            stopDvr();
        }
        try {
            dvrFd = getContentResolver().openFileDescriptor(dvrUri, "rw");
            videoPlayer.startDvr(dvrFd.getFd(), getDvrMP4());
            binding.imgBtnRecord.setImageResource(R.drawable.recording);
        } catch (IOException e) {
            Log.e(TAG, "Failed to open dvr file ", e);
            dvrFd = null;
        }

        binding.txtRecordLabel.setVisibility(View.VISIBLE);
        recordTimer = new Timer();
        recordTimer.schedule(new TimerTask() {
            @Override
            public void run() {
                int minutes = seconds / 60;
                int secs = seconds % 60;

                String timeFormatted = String.format("%02d:%02d", minutes, secs);
                runOnUiThread(() -> binding.txtRecordLabel.setText(timeFormatted));
                seconds++;
            }
        }, 0, 1000);

        dvrIconTimer = new Timer();
        dvrIconTimer.schedule(new TimerTask() {
            @Override
            public void run() {
                runOnUiThread(() -> binding.imgRecIndicator.setVisibility(binding.imgRecIndicator
                        .getVisibility() == View.VISIBLE ? View.INVISIBLE : View.VISIBLE));
            }
        }, 0, 1000);
    }

    private void stopDvr() {
        if (dvrFd == null) {
            return;
        }
        binding.imgRecIndicator.setVisibility(View.INVISIBLE);
        binding.imgBtnRecord.setImageResource(R.drawable.record);
        videoPlayer.stopDvr();
        if (recordTimer != null) {
            recordTimer.cancel();
            recordTimer.purge();
            recordTimer = null;
            seconds = 0;
            binding.txtRecordLabel.setVisibility(View.GONE);
        }
        if (dvrIconTimer != null) {
            dvrIconTimer.cancel();
            dvrIconTimer.purge();
            dvrIconTimer = null;
        }
        try {
            dvrFd.close();
        } catch (IOException e) {
            e.printStackTrace();
        }
        dvrFd = null;
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, @Nullable Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == PICK_KEY_REQUEST_CODE && resultCode == RESULT_OK) {
            if (data != null && data.getData() != null) {
                Uri uri = data.getData();
                Log.d(TAG, "Selected file " + uri);
                try {
                    InputStream inputStream = getContentResolver().openInputStream(uri);
                    setGsKey(inputStream);
                    copyGSKey();
                    wfbLinkManager.refreshKey();
                    inputStream.close();
                } catch (IOException e) {
                    Log.e(TAG, "Failed to import gs.key from " + uri);
                }
            }
        } else if (requestCode == PICK_DVR_REQUEST_CODE && resultCode == RESULT_OK) {
            // The result data contains a URI for the document or directory that
            // the user selected.
            Uri uri;
            if (data != null && data.getData() != null) {
                uri = data.getData();
                final int takeFlags = data.getFlags() &
                        (Intent.FLAG_GRANT_READ_URI_PERMISSION |
                                Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
                getContentResolver().takePersistableUriPermission(uri, takeFlags);

                // Perform operations on the document using its URI.
                SharedPreferences prefs = getSharedPreferences("general", Context.MODE_PRIVATE);
                SharedPreferences.Editor editor = prefs.edit();
                editor.putString("dvr_folder_", uri.toString());
                editor.apply();
                Uri dvrUri = openDvrFile();
                if (dvrUri != null) {
                    startDvr(dvrUri);
                }
            }
        } else if (requestCode == 100) {  // VPN_REQUEST_CODE is 100
            if (resultCode == RESULT_OK) {
                // VPN permission granted, start the VPN service
                Intent serviceIntent = new Intent(this, WfbNgVpnService.class);
                startService(serviceIntent);
            } else {
                // VPN permission not granted
                Log.e(TAG, "VPN permission was not granted by the user.");
            }
        }
    }

    public void setDefaultGsKey() {
        if (getGsKey().length > 0) {
            Log.d(TAG, "gs.key already saved in preferences.");
            return;
        }
        try {
            Log.d(TAG, "Importing default gs.key...");
            InputStream inputStream = getAssets().open("gs.key");
            setGsKey(inputStream);
            inputStream.close();
        } catch (IOException e) {
            Log.e(TAG, "Failed to import default gs.key");
        }
    }

    public byte[] getGsKey() {
        String pref = getSharedPreferences("general", Context.MODE_PRIVATE).getString("gs.key", "");
        return Base64.decode(pref, Base64.DEFAULT);
    }

    public void setGsKey(InputStream inputStream) throws IOException {
        ByteArrayOutputStream result = new ByteArrayOutputStream();
        byte[] buffer = new byte[1024];
        int length;
        while ((length = inputStream.read(buffer)) != -1) {
            result.write(buffer, 0, length);
        }
        SharedPreferences prefs = getSharedPreferences("general", Context.MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putString("gs.key", Base64.encodeToString(result.toByteArray(), Base64.DEFAULT));
        editor.apply();
    }

    public boolean getDvrMP4() {
        return getSharedPreferences("general", Context.MODE_PRIVATE).getBoolean("dvr_fmp4", true);
    }

    public void setDvrMP4(boolean enabled) {
        SharedPreferences prefs = getSharedPreferences("general", Context.MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putBoolean("dvr_fmp4", enabled);
        editor.apply();
    }

    @SuppressLint("UnspecifiedRegisterReceiverFlag")
    public void registerReceivers() {
        IntentFilter usbFilter = new IntentFilter();
        usbFilter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        usbFilter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        usbFilter.addAction(WfbLinkManager.ACTION_USB_PERMISSION);
        IntentFilter batFilter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);

        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(wfbLinkManager, usbFilter, Context.RECEIVER_NOT_EXPORTED);
            registerReceiver(batteryReceiver, batFilter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(wfbLinkManager, usbFilter);
            registerReceiver(batteryReceiver, batFilter);
        }
    }

    public void unregisterReceivers() {
        try {
            unregisterReceiver(wfbLinkManager);
        } catch (IllegalArgumentException ignored) {
        }
        try {
            unregisterReceiver(batteryReceiver);
        } catch (IllegalArgumentException ignored) {
        }
    }

    @Override
    protected void onPause() {
        super.onPause();

        unregisterReceivers();

        videoPlayer.stop();
        videoPlayer.stopAudio();
        wfbLinkManager.stopAdapters();
    }

    @Override
    protected void onStop() {
        MavlinkNative.nativeStop(this);
        handler.removeCallbacks(runnable);
        unregisterReceivers();
        wfbLinkManager.stopAdapters();
        videoPlayer.stop();
        videoPlayer.stopAudio();
        super.onStop();
    }

    @Override
    protected void onResume() {
        registerReceivers();

        wfbLinkManager.setChannel(getChannel(this));
        wfbLinkManager.setBandwidth(getBandwidth(this));

        // On resume is called when the app is reopened, a device might have been plugged since the last time it started.
        wfbLinkManager.refreshAdapters();

        wfbLinkManager.startAdapters();

        osdManager.restoreOSDConfig();

        videoPlayer.start();
        videoPlayer.startAudio();

        super.onResume();
    }

    @Override
    public void onChannelSettingChanged(int channel) {
        int currentChannel = getChannel(this);
        if (currentChannel == channel) {
            return;
        }
        SharedPreferences prefs = getSharedPreferences("general", Context.MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putInt("wifi-channel", channel);
        editor.apply();
        wfbLinkManager.stopAdapters();
        wfbLinkManager.setChannel(channel);
        wfbLinkManager.startAdapters();
    }

    @Override
    public void onBandwidthSettingChanged(int bandwidth) {
        int currentBandwidth = getBandwidth(this);
        if (currentBandwidth == bandwidth) {
            return;
        }
        SharedPreferences prefs = getSharedPreferences("general", Context.MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putInt("bandwidth", bandwidth);
        editor.apply();
        wfbLinkManager.stopAdapters();
        wfbLinkManager.setBandwidth(bandwidth);
        wfbLinkManager.startAdapters();
    }

    @Override
    public void onVideoRatioChanged(final int videoW, final int videoH) {
        lastVideoW = videoW;
        lastVideoH = videoH;

        Log.d(TAG, "Set resolution: " + videoW + "x" + videoH);

        updateViewRatio(R.id.mainVideo, lastVideoW, lastVideoH);
        updateViewRatio(R.id.surfaceViewLeft, lastVideoW, lastVideoH);
        updateViewRatio(R.id.surfaceViewRight, lastVideoW, lastVideoH);
    }

    private void updateViewRatio(int viewId, int videoW, int videoH) {
        if (videoW == 0 || videoH == 0) {
            return;
        }

        View view = findViewById(viewId);
        if (view != null) {
            ConstraintLayout.LayoutParams params =
                    (ConstraintLayout.LayoutParams) view.getLayoutParams();
            params.dimensionRatio = videoW + ":" + videoH;
            runOnUiThread(() -> view.setLayoutParams(params));
        } else {
            Log.w(TAG, "View with ID " + viewId + " not found.");
        }
    }

    @Override
    public void onDecodingInfoChanged(final DecodingInfo decodingInfo) {
        mDecodingInfo = decodingInfo;
        runOnUiThread(() -> {
            if (lastCodec != decodingInfo.nCodec) {
                lastCodec = decodingInfo.nCodec;
            }
            if (decodingInfo.currentFPS > 0) {
                binding.tvMessage.setVisibility(View.GONE);
                binding.wifiMessage.setVisibility(View.GONE);
            }
            String info = "%dx%d@%.0f " + (decodingInfo.nCodec == 1 ? " H265 " : " H264 ")
                    + (decodingInfo.currentKiloBitsPerSecond > 1000 ? " %.1fMbps " : " %.1fKpbs ")
                    + " %.1fms";
            binding.tvVideoStats.setText(String.format(Locale.US, info,
                    lastVideoW, lastVideoH, decodingInfo.currentFPS,
                    decodingInfo.currentKiloBitsPerSecond / 1000,
                    decodingInfo.avgTotalDecodingTime_ms));
        });
    }

    @Override
    public void onWfbNgStatsChanged(WfbNGStats data) {
        runOnUiThread(() -> {
            if (data.count_p_all > 0) {
                binding.tvMessage.setVisibility(View.INVISIBLE);
                binding.tvMessage.setText("");
                if (data.count_p_dec_err > 0) {
                    binding.tvLinkStatus.setText("Waiting for session key.");
                } else {
                    // NOTE: The order of the entries when being added to the entries array
                    // determines their position around the center of the chart.
                    ArrayList<PieEntry> entries = new ArrayList<>();
                    entries.add(new PieEntry((float) data.count_p_dec_ok / data.count_p_all));
                    entries.add(new PieEntry((float) data.count_p_fec_recovered / data.count_p_all));
                    entries.add(new PieEntry((float) data.count_p_lost / data.count_p_all));
                    PieDataSet dataSet = new PieDataSet(entries, "Link Status");
                    dataSet.setDrawIcons(false);
                    dataSet.setDrawValues(false);
                    ArrayList<Integer> colors = new ArrayList<>();
                    colors.add(getColor(R.color.colorGreen));
                    colors.add(getColor(R.color.colorYellow));
                    colors.add(getColor(R.color.colorRed));
                    dataSet.setColors(colors);
                    PieData pieData = new PieData(dataSet);
                    pieData.setValueFormatter(new PercentFormatter());
                    pieData.setValueTextSize(11f);
                    pieData.setValueTextColor(Color.WHITE);

                    binding.pcLinkStat.setData(pieData);
                    binding.pcLinkStat.setCenterText("" + data.count_p_fec_recovered);
                    binding.pcLinkStat.invalidate();

                    int color = getColor(R.color.colorGreenBg);
                    if ((float) data.count_p_fec_recovered / data.count_p_all > 0.2) {
                        color = getColor(R.color.colorYellowBg);
                    }
                    if (data.count_p_lost > 0) {
                        color = getColor(R.color.colorRedBg);
                    }
                    binding.imgLinkStatus.setImageTintList(ColorStateList.valueOf(color));
                    binding.tvLinkStatus.setText(String.format("O%sD%sR%sL%s",
                            paddedDigits(data.count_p_outgoing, 6),
                            paddedDigits(data.count_p_dec_ok, 6),
                            paddedDigits(data.count_p_fec_recovered, 6),
                            paddedDigits(data.count_p_lost, 6)));
                }
            } else {
                binding.tvLinkStatus.setText("No wfb-ng data.");
            }
        });
    }

    @Override
    public void onNewMavlinkData(MavlinkData data) {
        runOnUiThread(() -> osdManager.render(data));
    }

    private void copyGSKey() {
        File file = new File(getApplicationContext().getFilesDir(), "gs.key");
        OutputStream out = null;
        try {
            byte[] keyBytes = getGsKey();
            Log.d(TAG, "Using gs.key:" + bytesToHex(keyBytes) + "; Copying to" + file.getAbsolutePath());
            out = new FileOutputStream(file);
            out.write(keyBytes, 0, keyBytes.length);
        } catch (IOException e) {
            Log.e(TAG, "Failed to copy asset", e);
        } finally {
            if (out != null) {
                try {
                    out.close();
                } catch (IOException e) {
                    // NOOP
                }
            }
        }
    }

    @SuppressLint("SetJavaScriptEnabled")
    public void startBrowser() {
        WebView view = new WebView(this);
        view.setWebViewClient(new WebViewClient());
        view.getSettings().setJavaScriptEnabled(true);
        view.loadUrl("10.5.0.10");

        Dialog dialog = new Dialog(this);
        dialog.setContentView(view);
        dialog.setCanceledOnTouchOutside(true);

        DisplayMetrics displayMetrics = new DisplayMetrics();
        getWindowManager().getDefaultDisplay().getMetrics(displayMetrics);
        int screenWidth = (int) (displayMetrics.widthPixels * 0.75);
        if (dialog.getWindow() != null) {
            dialog.getWindow().setLayout(screenWidth, WindowManager.LayoutParams.MATCH_PARENT);
        }
        dialog.show();

        view.setWebViewClient(new WebViewClient() {
            @Override
            public void onReceivedHttpAuthRequest(
                    WebView view, HttpAuthHandler handler, String host, String realm) {
                handler.proceed("root", "12345");
            }
        });
    }
}
