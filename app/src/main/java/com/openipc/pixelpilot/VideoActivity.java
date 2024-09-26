package com.openipc.pixelpilot;

import android.annotation.SuppressLint;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.hardware.usb.UsbManager;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.BatteryManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.text.format.Formatter;
import android.util.Base64;
import android.util.Log;
import android.view.MenuItem;
import android.view.MotionEvent;
import android.view.SubMenu;
import android.view.View;
import android.view.WindowManager;
import android.widget.PopupMenu;
import android.widget.SeekBar;
import android.widget.SeekBar.OnSeekBarChangeListener;
import android.widget.Toast;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.constraintlayout.widget.ConstraintLayout;
import androidx.core.content.FileProvider;
import androidx.constraintlayout.widget.ConstraintSet;
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
    private boolean isStreaming = false;
    private ConstraintLayout constraintLayout;
    private ConstraintSet constraintSet;

    public boolean getVRSetting() {
        return getSharedPreferences("general", Context.MODE_PRIVATE).getBoolean("vr-mode", false);
    }

    public void setVRSetting(boolean v)
    {
        SharedPreferences prefs = getSharedPreferences("general", Context.MODE_PRIVATE);
        SharedPreferences.Editor editor = prefs.edit();
        editor.putBoolean("vr-mode", v);
        editor.apply();
    }

    public static int getChannel(Context context) {
        return context.getSharedPreferences("general",
                Context.MODE_PRIVATE).getInt("wifi-channel", 161);
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

    private void resetApp()
    {
        // Restart the app
        Intent intent = getPackageManager().getLaunchIntentForPackage(getPackageName());
        if (intent != null) {
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            startActivity(intent);
            finish();
            System.exit(0); // Ensure the app is fully restarted
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Log.d(TAG, "lifecycle onCreate");
        super.onCreate(savedInstanceState);
        binding = ActivityVideoBinding.inflate(getLayoutInflater());
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_HIDE_NAVIGATION);
        wifiManager = (WifiManager) getSystemService(WIFI_SERVICE);

        // Init wfb ng.
        setDefaultGsKey();
        copyGSKey();
        WfbNgLink wfbLink = new WfbNgLink(this);
        wfbLink.SetWfbNGStatsChanged(this);
        wfbLinkManager = new WfbLinkManager(this, binding, wfbLink);

        // Setup video players
        setContentView(binding.getRoot());
        videoPlayer = new VideoPlayer(this);
        videoPlayer.setIVideoParamsChanged(this);
        isVRMode = getVRSetting();
        if(isVRMode) {
            binding.mainVideo.setVisibility(View.GONE);
            binding.surfaceViewLeft.getHolder().addCallback(videoPlayer.configure1(0));
            binding.surfaceViewRight.getHolder().addCallback(videoPlayer.configure1(1));

            SeekBar seekBar = binding.seekBar;
            // Retrieve saved progress value
            SharedPreferences sharedPreferences = getSharedPreferences("SeekBarPrefs", MODE_PRIVATE);
            int savedProgress = sharedPreferences.getInt("seekBarProgress", 0); // Default to 0 if no value is found
            seekBar.setProgress(savedProgress);
            seekBar.setVisibility(View.VISIBLE);
            constraintLayout = binding.frameLayout;
            constraintSet = new ConstraintSet();
            constraintSet.clone(constraintLayout);

            // Apply the saved margin
            int margin = savedProgress * 10; // Adjust the multiplier as needed
            constraintSet.setMargin(R.id.surfaceViewLeft, ConstraintSet.END, margin);
            constraintSet.setMargin(R.id.surfaceViewRight, ConstraintSet.START, margin);
            constraintSet.applyTo(constraintLayout);

            // Hide SeekBar after 3 seconds
            handler.postDelayed(new Runnable() {
                @Override
                public void run() {
                    seekBar.setVisibility(View.GONE);
                }
            }, 3000);

            // Show SeekBar when touched
            constraintLayout.setOnTouchListener(new View.OnTouchListener() {
                @Override
                public boolean onTouch(View v, MotionEvent event) {
                    if (event.getAction() == MotionEvent.ACTION_DOWN) {
                        seekBar.setVisibility(View.VISIBLE);
                        // Hide SeekBar again after 3 seconds of inactivity
                        handler.postDelayed(new Runnable() {
                            @Override
                            public void run() {
                                seekBar.setVisibility(View.GONE);
                            }
                        }, 3000);
                    }
                    return false;
                }
            });

            seekBar.setOnSeekBarChangeListener(new OnSeekBarChangeListener() {
                @Override
                public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                    int margin = progress * 10; // Adjust the multiplier as needed
                    constraintSet.setMargin(R.id.surfaceViewLeft, ConstraintSet.END, margin);
                    constraintSet.setMargin(R.id.surfaceViewRight, ConstraintSet.START, margin);
                    constraintSet.applyTo(constraintLayout);
                    // Save progress value
                    SharedPreferences sharedPreferences = getSharedPreferences("SeekBarPrefs", MODE_PRIVATE);
                    SharedPreferences.Editor editor = sharedPreferences.edit();
                    editor.putInt("seekBarProgress", progress);
                    editor.apply();
                }

                @Override
                public void onStartTrackingTouch(SeekBar seekBar) {

                }

                @Override
                public void onStopTrackingTouch(SeekBar seekBar) {

                }
            });
        }
        else {
            binding.surfaceViewRight.setVisibility(View.GONE);
            binding.surfaceViewLeft.setVisibility(View.GONE);
            binding.mainVideo.getHolder().addCallback(videoPlayer.configure1(0));
        }

        osdManager = new OSDManager(this, binding);
        osdManager.setUp();

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
        PieData noData = new PieData(new PieDataSet(new ArrayList<>(), ""));
        chart.setData(noData);

        binding.imgBtnRecord.setOnClickListener(item -> {
            if(!isStreaming) return;

            if (dvrFd == null) {
                Uri dvrUri = openDvrFile();
                if (dvrUri != null) {
                    startDvr(dvrUri);
                } else {
                    Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                    intent.addCategory(Intent.CATEGORY_DEFAULT);
                    startActivityForResult(intent, PICK_DVR_REQUEST_CODE);
                }
            } else {
                stopDvr();
            }
        });

        binding.btnSettings.setOnClickListener(v -> {
            PopupMenu popup = new PopupMenu(this, v);
            SubMenu vrMenu = popup.getMenu().addSubMenu("VR mode");
            MenuItem vrItem = vrMenu.add(getVRSetting() ? "On" : "Off");
            vrItem.setOnMenuItemClickListener(item -> {
                isVRMode = !getVRSetting();
                setVRSetting(isVRMode);
                vrItem.setTitle(isVRMode ? "On" : "Off");
                item.setShowAsAction(MenuItem.SHOW_AS_ACTION_COLLAPSE_ACTION_VIEW);
                item.setActionView(new View(this));
                resetApp();
                return false;
            });

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

            // OSD
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
                itm.setOnMenuItemClickListener(item -> {
                    item.setChecked(!item.isChecked());
                    osdManager.onOSDItemCheckChanged(element, item.isChecked());
                    item.setShowAsAction(MenuItem.SHOW_AS_ACTION_COLLAPSE_ACTION_VIEW);
                    item.setActionView(new View(this));
                    return false;
                });
            }

            // WFB
            SubMenu wfb = popup.getMenu().addSubMenu("WFB-NG");
            MenuItem keyBtn = wfb.add("gs.key");
            keyBtn.setOnMenuItemClickListener(item -> {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                startActivityForResult(intent, PICK_KEY_REQUEST_CODE);
                return true;
            });

            // Recording
            SubMenu recording = popup.getMenu().addSubMenu("Recording");
            MenuItem dvrBtn = recording.add(dvrFd == null ? "Start" : "Stop");
            dvrBtn.setOnMenuItemClickListener(item -> {
                if(!isStreaming) return false;
                if (dvrFd == null) {
                    Uri dvrUri = openDvrFile();
                    if (dvrUri != null) {
                        startDvr(dvrUri);
                    } else {
                        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
                        intent.addCategory(Intent.CATEGORY_DEFAULT);
                        startActivityForResult(intent, PICK_DVR_REQUEST_CODE);
                    }
                } else {
                    stopDvr();
                }
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

            SubMenu help = popup.getMenu().addSubMenu("Help");
            MenuItem logs = help.add("Send Logs");
            logs.setOnMenuItemClickListener(item -> {
                try {
                    Process process = Runtime.getRuntime().exec("logcat -d");
                    BufferedReader bufferedReader = new BufferedReader(new InputStreamReader(process.getInputStream()));

                    String timeStamp = new SimpleDateFormat("yyyyMMdd_HHmmss").format(new Date());
                    File logFile = new File(getExternalFilesDir(null), "pixelpilot_log_" + timeStamp + ".txt");
                    FileWriter fileWriter = new FileWriter(logFile);

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
                    } catch (PackageManager.NameNotFoundException e) {
                    }

                    fileWriter.append("Device Model: " + Build.MODEL + "\n" +
                            "Manufacturer: " + Build.MANUFACTURER + "\n" +
                            "OS Version: " + Build.VERSION.RELEASE + "\n" +
                            "SDK Version: " + Build.VERSION.SDK_INT + "\n" +
                            "App Version Name: " + versionName + "\n" +
                            "App Version Code: " + versionCode + "\n");

                    String line;

                    while ((line = bufferedReader.readLine()) != null) {
                        fileWriter.append(line).append("\n");
                    }
                    fileWriter.flush();
                    fileWriter.close();
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
                return true;
            });

            popup.show();
        });

        // Setup mavlink
        MavlinkNative.nativeStart(this);
        handler.post(runnable);

        batteryReceiver = new BroadcastReceiver() {
            public void onReceive(Context context, Intent batteryStatus) {
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
                    if (batteryPct <= 0) {
                        icon = R.drawable.baseline_battery_0_bar_24;
                    } else if (batteryPct <= 1 / 7.0 * 100) {
                        icon = R.drawable.baseline_battery_1_bar_24;
                    } else if (batteryPct <= 2 / 7.0 * 100) {
                        icon = R.drawable.baseline_battery_2_bar_24;
                    } else if (batteryPct <= 3 / 7.0 * 100) {
                        icon = R.drawable.baseline_battery_3_bar_24;
                    } else if (batteryPct <= 4 / 7.0 * 100) {
                        icon = R.drawable.baseline_battery_4_bar_24;
                    } else if (batteryPct <= 5 / 7.0 * 100) {
                        icon = R.drawable.baseline_battery_5_bar_24;
                    } else if (batteryPct <= 6 / 7.0 * 100) {
                        icon = R.drawable.baseline_battery_6_bar_24;
                    } else {
                        icon = R.drawable.baseline_battery_full_24;
                    }
                }
                binding.imgGSBattery.setImageResource(icon);
            }
        };
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
            if(newFile == null)
                Log.e(TAG, "dvr newFile null");
            return newFile != null ? newFile.getUri() : null;
        }
        return null;
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
        if(recordTimer != null) {
            recordTimer.cancel();
            recordTimer.purge();
            recordTimer = null;
            seconds = 0;
            binding.txtRecordLabel.setVisibility(View.GONE);
        }
        if(dvrIconTimer != null) {
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
    }

    @Override
    protected void onStop() {
        MavlinkNative.nativeStop(this);
        handler.removeCallbacks(runnable);
        unregisterReceivers();
        wfbLinkManager.stopAdapters();
        videoPlayer.stop();
        super.onStop();
    }

    @Override
    protected void onResume() {
        registerReceivers();

        // On resume is called when the app is reopened, a device might have been plugged since the last time it started.
        videoPlayer.start();

        wfbLinkManager.setChannel(getChannel(this));
        wfbLinkManager.refreshAdapters();
        osdManager.restoreOSDConfig();

        registerReceivers();
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
        wfbLinkManager.startAdapters(channel);
    }

    @Override
    public void onVideoRatioChanged(final int videoW, final int videoH) {
        lastVideoW = videoW;
        lastVideoH = videoH;

        Log.d(TAG, "Set resolution: " + videoW + "x" + videoH);
        View mSurface = findViewById(R.id.mainVideo);
        ConstraintLayout.LayoutParams params =
                (ConstraintLayout.LayoutParams) mSurface.getLayoutParams();
        params.dimensionRatio = videoW + ":" + videoH;
        runOnUiThread(() -> mSurface.setLayoutParams(params));
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
                    isStreaming = true;
                }
            } else {
                binding.tvLinkStatus.setText("No wfb-ng data.");
                isStreaming = false;
                binding.imgBtnRecord.setImageResource(R.drawable.record);
                stopDvr();
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
}
