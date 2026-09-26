console.log('[+] JSAgent: Starting...');

function onJavaReady(Java) {
    console.log('[+] JSAgent: Java is ready! Inside Java.perform()...');

    // Now you can use Java.use()
    try {
        const Display = Java.use('android.view.Display');
        console.log('[+] JSAgent: Successfully loaded Display class');
        console.log('[+] JSAgent: Display methods: ' + Object.keys(Display).join(', '));
    } catch (e) {
        console.error('[!] JSAgent: Failed to load Display: ' + e.toString());
    }

    function getRealisticJitter(baseValue, variancePercent) {
        const variance = baseValue * variancePercent;
        const offset = (Math.random() * variance * 2) - variance;
        return Math.floor(baseValue + offset);
    }

    console.log('[+] JSAgent: Jitter utility ready');
    // Add your Frida Java hooks here
}

function waitForJava() {
    let attempts = 0;

    const interval = setInterval(function () {
        attempts++;
        
        try {
            const Java = require("frida-java-bridge");

            // Key: Check if we can actually USE Java, not just if the module exists
            if (Java && typeof Java.available !== 'undefined' && Java.available === true) {
                clearInterval(interval);
                console.log('[+] JSAgent: Java.available === true at attempt ' + attempts);

                Java.perform(function () {
                    onJavaReady(Java);
                });
                return;
            }

            // Log progress
            if (attempts % 100 === 0) {
                console.log('[+] JSAgent: Attempt ' + attempts + ' - Java.available: ' + Java.available);
            }
        } catch (e) {
            if (attempts === 1) {
                console.error('[!] JSAgent: Error: ' + e.toString());
            }
        }

        // 300 second timeout (6 minutes)
        if (attempts > 6000) {
            clearInterval(interval);
            console.error('[!] JSAgent: FATAL - Java never initialized after 300 seconds');
        }
    }, 50);
}

console.log('[+] JSAgent: Waiting for Java to initialize...');
waitForJava();