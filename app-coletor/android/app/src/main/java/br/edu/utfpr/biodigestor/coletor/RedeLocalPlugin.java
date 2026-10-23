package br.edu.utfpr.biodigestor.coletor;

import android.content.Context;
import android.content.Intent;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.PatternMatcher;
import android.provider.Settings;

import com.getcapacitor.JSObject;
import com.getcapacitor.Plugin;
import com.getcapacitor.PluginCall;
import com.getcapacitor.PluginMethod;
import com.getcapacitor.annotation.CapacitorPlugin;

import java.util.concurrent.atomic.AtomicBoolean;

/**
 * Plugin "RedeLocal" — resolve o problema do Wi-Fi sem internet.
 *
 * Quando o celular está no AP do ESP (sem internet) e os dados móveis estão ligados, o Android
 * costuma mandar o tráfego do app pelos dados móveis e 192.168.4.1 fica inalcançável.
 * vincularWifi() chama bindProcessToNetwork(rede Wi-Fi) e todo o tráfego do processo (inclusive o
 * HTTP nativo do Capacitor) passa a sair pelo Wi-Fi; desvincular() devolve ao padrão do sistema.
 *
 * conectarAp() (Android 10+) pede ao sistema a conexão ao AP "Biodigestor-…" com WifiNetworkSpecifier;
 * o usuário confirma numa janela do Android. A rede fica ativa enquanto o app mantiver o pedido.
 */
@CapacitorPlugin(name = "RedeLocal")
public class RedeLocalPlugin extends Plugin {

    private ConnectivityManager.NetworkCallback callbackAp; // pedido WifiNetworkSpecifier ativo
    private volatile Network redeAp;                         // rede obtida pelo pedido acima
    private volatile boolean vinculado = false;

    private ConnectivityManager cm() {
        return (ConnectivityManager) getContext().getApplicationContext().getSystemService(Context.CONNECTIVITY_SERVICE);
    }

    /** Rede Wi-Fi atual: a do pedido do app, se houver; senão a primeira rede Wi-Fi conectada. */
    @SuppressWarnings("deprecation")
    private Network acharWifi() {
        ConnectivityManager cm = cm();
        Network ap = redeAp;
        if (ap != null && cm.getNetworkCapabilities(ap) != null) return ap;
        for (Network n : cm.getAllNetworks()) {
            NetworkCapabilities c = cm.getNetworkCapabilities(n);
            if (c != null && c.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return n;
        }
        return null;
    }

    @SuppressWarnings("deprecation")
    private boolean vincular(Network n) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) return cm().bindProcessToNetwork(n);
        return ConnectivityManager.setProcessDefaultNetwork(n);
    }

    private boolean temInternet(Network n) {
        if (n == null || Build.VERSION.SDK_INT < Build.VERSION_CODES.M) return false;
        NetworkCapabilities c = cm().getNetworkCapabilities(n);
        return c != null && c.hasCapability(NetworkCapabilities.NET_CAPABILITY_VALIDATED);
    }

    @PluginMethod
    public void estado(PluginCall call) {
        Network w = acharWifi();
        JSObject r = new JSObject();
        r.put("wifi", w != null);
        r.put("wifiTemInternet", temInternet(w));
        r.put("vinculado", vinculado);
        r.put("apConectadoPeloApp", redeAp != null);
        r.put("android", Build.VERSION.SDK_INT);
        call.resolve(r);
    }

    @PluginMethod
    public void vincularWifi(PluginCall call) {
        Network w = acharWifi();
        if (w == null) {
            call.reject("Nenhuma rede Wi-Fi conectada. Conecte-se à rede \"Biodigestor-XXXX\".", "SEM_WIFI");
            return;
        }
        boolean ok = vincular(w);
        vinculado = ok;
        JSObject r = new JSObject();
        r.put("ok", ok);
        r.put("temInternet", temInternet(w));
        call.resolve(r);
    }

    @PluginMethod
    public void desvincular(PluginCall call) {
        vincular(null);
        vinculado = false;
        JSObject r = new JSObject();
        r.put("ok", true);
        call.resolve(r);
    }

    @PluginMethod
    public void conectarAp(final PluginCall call) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
            call.reject("A conexão automática exige Android 10 ou mais novo.", "NAO_SUPORTADO");
            return;
        }
        String ssid = call.getString("ssid", "");
        String senha = call.getString("senha", "");
        int tempoMs = call.getInt("tempoMs", 45000);
        liberarAp();

        WifiNetworkSpecifier.Builder esp = new WifiNetworkSpecifier.Builder();
        if (ssid == null || ssid.trim().isEmpty() || ssid.toUpperCase().endsWith("XXXX")) {
            esp.setSsidPattern(new PatternMatcher("Biodigestor-", PatternMatcher.PATTERN_PREFIX));
        } else {
            esp.setSsid(ssid.trim());
        }
        if (senha != null && senha.length() >= 8) esp.setWpa2Passphrase(senha);

        NetworkRequest pedido = new NetworkRequest.Builder()
                .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
                .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET)
                .setNetworkSpecifier(esp.build())
                .build();

        final AtomicBoolean respondido = new AtomicBoolean(false);
        ConnectivityManager.NetworkCallback cb = new ConnectivityManager.NetworkCallback() {
            @Override
            public void onAvailable(Network n) {
                redeAp = n;
                if (respondido.compareAndSet(false, true)) {
                    JSObject r = new JSObject();
                    r.put("ok", true);
                    call.resolve(r);
                }
            }

            @Override
            public void onUnavailable() {
                if (callbackAp == this) callbackAp = null;
                if (respondido.compareAndSet(false, true)) {
                    call.reject("Não foi possível conectar à rede do biodigestor (cancelado, senha errada ou fora de alcance).", "INDISPONIVEL");
                }
            }

            @Override
            public void onLost(Network n) {
                if (n.equals(redeAp)) {
                    redeAp = null;
                    if (vinculado) {
                        vincular(null);
                        vinculado = false;
                    }
                    notifyListeners("apPerdido", new JSObject());
                }
            }
        };
        callbackAp = cb;
        try {
            cm().requestNetwork(pedido, cb, tempoMs);
        } catch (Exception e) {
            callbackAp = null;
            if (respondido.compareAndSet(false, true)) call.reject("O Android recusou o pedido de conexão: " + e.getMessage(), "RECUSADO");
        }
    }

    @PluginMethod
    public void desconectarAp(PluginCall call) {
        liberarAp();
        JSObject r = new JSObject();
        r.put("ok", true);
        call.resolve(r);
    }

    @PluginMethod
    public void abrirConfigWifi(PluginCall call) {
        Intent i = Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q
                ? new Intent(Settings.Panel.ACTION_WIFI)
                : new Intent(Settings.ACTION_WIFI_SETTINGS);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        JSObject r = new JSObject();
        try {
            getContext().startActivity(i);
            r.put("ok", true);
        } catch (Exception e) {
            try {
                Intent j = new Intent(Settings.ACTION_WIFI_SETTINGS);
                j.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
                getContext().startActivity(j);
                r.put("ok", true);
            } catch (Exception e2) {
                r.put("ok", false);
            }
        }
        call.resolve(r);
    }

    /** Solta o pedido de rede do AP (o Android desconecta dele) e desfaz o vínculo do processo. */
    private void liberarAp() {
        ConnectivityManager.NetworkCallback cb = callbackAp;
        callbackAp = null;
        redeAp = null;
        if (vinculado) {
            vincular(null);
            vinculado = false;
        }
        if (cb != null) {
            try {
                cm().unregisterNetworkCallback(cb);
            } catch (Exception ignorado) {
                // já liberado pelo sistema
            }
        }
    }

    @Override
    protected void handleOnDestroy() {
        liberarAp();
        super.handleOnDestroy();
    }
}
