package br.edu.utfpr.biodigestor.coletor;

import android.os.Bundle;
import com.getcapacitor.BridgeActivity;

public class MainActivity extends BridgeActivity {
    @Override
    public void onCreate(Bundle savedInstanceState) {
        // Plugins locais precisam ser registrados antes de super.onCreate
        registerPlugin(RedeLocalPlugin.class);
        super.onCreate(savedInstanceState);
    }
}
