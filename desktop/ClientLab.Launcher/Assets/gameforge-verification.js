// The publisher's widget owns the work, challenge submission, and verification UI.
// This adapter only loads it and checks the outcome from its own iframe.
globalThis.ClientLabGameforgeVerification = (() => {
  const origin = 'https://pow-captcha.gameforge.com';
  let loading;
  function load() {
    if (typeof window['gf-pow-captcha'] === 'function') return Promise.resolve();
    if (!loading) loading = new Promise((resolve, reject) => {
      const script = document.createElement('script');
      const timeout = setTimeout(() => { script.remove(); reject(new Error('Loading Gameforge verification timed out.')); }, 20000);
      script.src = origin + '/js/index.js';
      script.onload = () => { clearTimeout(timeout); typeof window['gf-pow-captcha'] === 'function' ? resolve() : reject(new Error('Verification widget is unavailable.')); };
      script.onerror = () => { clearTimeout(timeout); script.remove(); reject(new Error('Could not load Gameforge verification.')); };
      document.head.appendChild(script);
    }).catch(error => { loading = null; throw error; });
    return loading;
  }
  async function run(challengeId) {
    await load();
    const host = document.createElement('div');
    document.body.appendChild(host);
    const status = document.querySelector('main');
    if (status) status.hidden = true;
    let outcome;
    const receive = event => {
      const frame = host.querySelector('iframe');
      if (event.origin !== origin || !frame || event.source !== frame.contentWindow) return;
      if (['pow-captcha-solved', 'pow-captcha-failed', 'pow-captcha-canceled'].includes(event.data?.type)) outcome = event.data.type;
    };
    window.addEventListener('message', receive);
    let timer;
    try {
      await Promise.race([
        window['gf-pow-captcha'](host, { challengeId, locale: 'en-GB' }),
        new Promise((_, reject) => { timer = setTimeout(() => reject(new Error('Verification timed out. Please sign in again.')), 185000); })
      ]);
      // The official widget resolves its promise on both success and failure.
      if (outcome !== 'pow-captcha-solved') throw new Error('Gameforge verification failed. Please sign in again.');
    } catch (error) {
      if (outcome === 'pow-captcha-canceled' || error.name === 'CaptchaCanceledError') {
        const canceled = new Error('Verification cancelled.'); canceled.name = 'AbortError'; throw canceled;
      }
      throw error;
    } finally {
      clearTimeout(timer);
      window.removeEventListener('message', receive);
      host.remove();
      if (status) status.hidden = false;
    }
  }
  return { run };
})();
