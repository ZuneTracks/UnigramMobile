namespace Unigram {
namespace Native {
namespace Calls {
namespace Proof {
namespace {
__declspec(thread) volatile long g_tls_anchor = 0;
}

void EnsureUwpTlsSupport() {
    (void)g_tls_anchor;
}
}
}
}
}
