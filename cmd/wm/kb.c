/*
 * (C)opyright MMIV-MMVI Anselm R. Garbe <garbeam at gmail dot com>
 * See LICENSE file for license details.
 */

#include <string.h>
#include <stdlib.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

#include "wm.h"

#define NUM_MASKS      8

void
init_lock_modifiers()
{
    XModifierKeymap *modmap;
    KeyCode num_lock;
    static int masks[NUM_MASKS] = {
        ShiftMask, LockMask, ControlMask, Mod1Mask,
        Mod2Mask, Mod3Mask, Mod4Mask, Mod5Mask
    };
    int i;

    num_lock_mask = 0;
    modmap = XGetModifierMapping(dpy);
    num_lock = XKeysymToKeycode(dpy, XStringToKeysym("Num_Lock"));

    if(modmap && modmap->max_keypermod > 0) {
        int max = NUM_MASKS * modmap->max_keypermod;
        for(i = 0; i < max; i++) {
            if(num_lock && (modmap->modifiermap[i] == num_lock)) {
                num_lock_mask = masks[i / modmap->max_keypermod];
            }
        }
    }
    XFreeModifiermap(modmap);

    valid_mask = 255 & ~(num_lock_mask | LockMask);
}

static unsigned long
blitz_strtomod(char *val)
{
	unsigned long mod = 0;
	if (strstr(val, "Shift"))
		mod |= ShiftMask;
	if (strstr(val, "Control"))
		mod |= ControlMask;
	if (strstr(val, "Mod1") || strstr(val, "Alt"))
		mod |= Mod1Mask;
	if (strstr(val, "Mod2"))
		mod |= Mod2Mask;
	if (strstr(val, "Mod3"))
		mod |= Mod3Mask;
	if (strstr(val, "Mod4"))
		mod |= Mod4Mask;
	if (strstr(val, "Mod5"))
		mod |= Mod5Mask;
	return mod;
}

void
grab_key(Key *k)
{
    XGrabKey(dpy, k->key, k->mod, root,
             True, GrabModeAsync, GrabModeAsync);
    if(num_lock_mask) {
        XGrabKey(dpy, k->key, k->mod | num_lock_mask, root,
                 True, GrabModeAsync, GrabModeAsync);
        XGrabKey(dpy, k->key, k->mod | num_lock_mask | LockMask, root,
                 True, GrabModeAsync, GrabModeAsync);
    }
    XSync(dpy, False);
}

void
ungrab_key(Key *k)
{
    XUngrabKey(dpy, k->key, k->mod, root);
    if(num_lock_mask) {
        XUngrabKey(dpy, k->key, k->mod | num_lock_mask, root);
        XUngrabKey(dpy, k->key, k->mod | num_lock_mask | LockMask, root);
    }
    XSync(dpy, False);
}

Key *
create_key(char *name)
{
	char buf[128];
    char *seq[8];
    char *kstr;
    size_t i, toks;
	static unsigned short id = 1;
    Key *k = 0, *r = 0;

	if((k = name_to_key(name)))
		return k;

    cext_strlcpy(buf, name, sizeof(buf));
    toks = cext_tokenize(seq, 8, buf, ',');

    for(i = 0; i < toks; i++) {
        if(!k)
            r = k = cext_emallocz(sizeof(Key));
        else {
            k->next = cext_emallocz(sizeof(Key));
            k = k->next;
        }
        cext_strlcpy(k->name, name, sizeof(k->name));
        kstr = strrchr(seq[i], '-');
        if(kstr)
            kstr++;
        else
            kstr = seq[i];
        k->key = XKeysymToKeycode(dpy, XStringToKeysym(kstr));
        k->mod = blitz_strtomod(seq[i]);
    }
	r->id = id++;
	key = (Key **)cext_array_attach((void **)key, r, sizeof(Key *), &keysz);
	nkey++;
	return r;
}

void
destroy_key(Key *k)
{
	cext_array_detach((void **)key, k, &keysz);
	nkey--;
    if(k->next)
        destroy_key(k->next);
    free(k);
}

static void
next_keystroke(unsigned long *mod, KeyCode *key)
{
    XEvent e;
    KeySym sym;
    *mod = 0;
    do {
        XMaskEvent(dpy, KeyPressMask, &e);
        *mod |= e.xkey.state & valid_mask;
        *key = (KeyCode) e.xkey.keycode;
        sym = XKeycodeToKeysym(dpy, e.xkey.keycode, 0);
    } while(IsModifierKey(sym));
}

static void
emulate_key_press(unsigned long mod, KeyCode key)
{
    XEvent e;
    Window client_win;
    int revert;

    XGetInputFocus(dpy, &client_win, &revert);

    e.xkey.type = KeyPress;
    e.xkey.time = CurrentTime;
    e.xkey.window = client_win;
    e.xkey.display = dpy;
    e.xkey.state = mod;
    e.xkey.keycode = key;
    XSendEvent(dpy, client_win, True, KeyPressMask, &e);
    e.xkey.type = KeyRelease;
    XSendEvent(dpy, client_win, True, KeyReleaseMask, &e);
    XSync(dpy, False);
}

static Key **
match_keys(Key **t, size_t n, unsigned long mod, KeyCode keycode, Bool next, size_t *nres)
{
	Key **result = nil;
	size_t ressz = 0;
	size_t i = 0;
	*nres = 0;
	for(i = 0; i < n; i++) {
		Key *k = next ? t[i]->next : t[i];
		if(k && (k->mod == mod) && (k->key == keycode)) {
			result = (Key **)cext_array_attach((void **)result, k, sizeof(Key *), &ressz);
			(*nres)++;
		}
	}
	return result;
}

static void
handle_key_seq(Window w, Key **done, size_t ndone)
{
    unsigned long mod;
    KeyCode key;
	Key **found = nil;
	size_t nfound = 0; 
	char buf[128];

    next_keystroke(&mod, &key);

	found = match_keys(done, ndone, mod, key, True, &nfound);
	if((done[0]->mod == mod) && (done[0]->key == key))
		emulate_key_press(mod, key); /* double key */
	else {
		switch(nfound) {
		case 0:
			XBell(dpy, 0);
			return; /* grabbed but not found */
		case 1: 
			if(!found[0]->next) {
				snprintf(buf, sizeof(buf), "K %s\n", found[0]->name);
				broadcast_event(buf);
				break;
			}
		default:
			handle_key_seq(w, found, nfound);
			break;
		}
	}
	free(found);
}

void
handle_key(Window w, unsigned long mod, KeyCode keycode)
{
	size_t nfound;
	char buf[128];
	Key **found = match_keys(key, nkey, mod, keycode, False, &nfound);
	switch(nfound) {
	case 0:
		XBell(dpy, 0);
		return; /* grabbed but not found */
	case 1: 
		if(!found[0]->next) {
			snprintf(buf, sizeof(buf), "K %s\n", found[0]->name);
			broadcast_event(buf);
			break;
		}
	default:
		XGrabKeyboard(dpy, w, True, GrabModeAsync, GrabModeAsync, CurrentTime);
		handle_key_seq(w, found, nfound);
		XUngrabKeyboard(dpy, CurrentTime);
		XSync(dpy, False);
		break;
    }
	free(found);
}

Key *
name_to_key(char *name)
{
	size_t i;
	for(i = 0; i < nkey; i++)
		if(!strncmp(key[i]->name, name, sizeof(key[i]->name)))
			return key[i];
	return nil;
}

int
kid_to_index(unsigned short id)
{
	int i;
	for(i = 0; i < nkey; i++)
		if(key[i]->id == id)
			return i;
	return -1;
}