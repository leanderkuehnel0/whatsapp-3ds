package main

import (
	"context"
	"database/sql"
	"errors"
	"fmt"
	"log"
	"strings"
	"time"
	"unicode/utf8"

	"go.mau.fi/whatsmeow"
	"go.mau.fi/whatsmeow/proto/waE2E"
	"go.mau.fi/whatsmeow/types"
	"go.mau.fi/whatsmeow/types/events"
	"google.golang.org/protobuf/proto"
)

// Bridge ties the WhatsApp client, the message store and the connected 3DS clients together.
type Bridge struct {
	wa    *whatsmeow.Client
	store *Store
	hub   *Hub
	media *MediaCache
	calls *CallHub
}

func (b *Bridge) handleEvent(rawEvt any) {
	// Call signaling must not wait behind slow work like history sync.
	if isCallEvent(rawEvt) {
		go b.calls.HandleEvent(context.Background(), rawEvt)
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	switch evt := rawEvt.(type) {
	case *events.Message:
		b.handleMessage(ctx, evt, true)
	case *events.HistorySync:
		b.handleHistorySync(ctx, evt)
	case *events.Connected:
		log.Println("WhatsApp connected")
		b.hub.Broadcast(line("*", "STATUS", "connected"))
		go func() {
			b.refreshGroupNames()
			b.cleanupChats(true)
		}()
	case *events.Disconnected:
		log.Println("WhatsApp disconnected")
		b.hub.Broadcast(line("*", "STATUS", "disconnected"))
	case *events.LoggedOut:
		log.Println("WhatsApp session was logged out; restart the bridge to link again")
		b.hub.Broadcast(line("*", "STATUS", "logged_out"))
	case *events.GroupInfo:
		if evt.Name != nil {
			b.renameChat(ctx, evt.JID.String(), evt.Name.Name)
		}
	case *events.Contact:
		if evt.Action != nil {
			name := evt.Action.GetFullName()
			if name == "" {
				name = evt.Action.GetFirstName()
			}
			if name != "" {
				b.renameIfKnown(ctx, b.normalizeChat(ctx, evt.JID).String(), name, false)
			}
		}
	case *events.PushName:
		if evt.NewPushName != "" {
			b.renameIfKnown(ctx, b.normalizeChat(ctx, evt.JID).String(), evt.NewPushName, true)
		}
	case *events.AppStateSyncComplete:
		// contact names may have arrived: merge LID chats and let clients refetch
		go b.cleanupChats(true)
	case *events.MarkChatAsRead:
		if evt.Action.GetRead() {
			b.setUnread(ctx, b.normalizeChat(ctx, evt.JID).String(), 0)
		}
	default:
		b.calls.HandleEvent(ctx, rawEvt)
	}
}

// normalizeChat strips device parts and maps hidden LID user IDs to phone-number JIDs when known,
// so that one person always maps to one chat.
func (b *Bridge) normalizeChat(ctx context.Context, jid types.JID) types.JID {
	jid = jid.ToNonAD()
	if jid.Server == types.HiddenUserServer {
		if pn, err := b.wa.Store.LIDs.GetPNForLID(ctx, jid); err == nil && !pn.IsEmpty() {
			return pn.ToNonAD()
		}
	}
	return jid
}

func skipChat(jid types.JID) bool {
	return jid.Server == types.BroadcastServer || jid.Server == types.NewsletterServer || jid.IsEmpty()
}

// contactName picks the best display name for a user JID. When no name is known it returns a
// placeholder (phone number, redacted number or "~id") that isFallbackName recognises.
func (b *Bridge) contactName(ctx context.Context, jid types.JID) string {
	jid = jid.ToNonAD()
	pn := types.EmptyJID
	lookup := []types.JID{jid}
	if jid.Server == types.HiddenUserServer {
		if p, err := b.wa.Store.LIDs.GetPNForLID(ctx, jid); err == nil && !p.IsEmpty() {
			pn = p.ToNonAD()
			lookup = append(lookup, pn)
		}
	} else if jid.Server == types.DefaultUserServer {
		pn = jid
		if lid, err := b.wa.Store.LIDs.GetLIDForPN(ctx, jid); err == nil && !lid.IsEmpty() {
			lookup = append(lookup, lid.ToNonAD())
		}
	}
	redacted := ""
	// prefer the address-book name from any alias over push names
	for _, pick := range []func(types.ContactInfo) string{
		func(c types.ContactInfo) string { return c.FullName },
		func(c types.ContactInfo) string { return c.FirstName },
		func(c types.ContactInfo) string { return c.PushName },
		func(c types.ContactInfo) string { return c.BusinessName },
	} {
		for _, j := range lookup {
			c, err := b.wa.Store.Contacts.GetContact(ctx, j)
			if err != nil || !c.Found {
				continue
			}
			if n := strings.TrimSpace(pick(c)); n != "" {
				return n
			}
			if c.RedactedPhone != "" {
				redacted = c.RedactedPhone
			}
		}
	}
	switch {
	case !pn.IsEmpty():
		return "+" + pn.User
	case redacted != "":
		return redacted
	}
	return "~" + jid.User
}

// isFallbackName reports whether a stored name is only a placeholder that should be
// looked up again once contacts have synced.
func isFallbackName(s string) bool {
	if s == "" || s == "Group" || strings.HasPrefix(s, "+") || strings.HasPrefix(s, "~") {
		return true
	}
	for _, r := range s {
		if r < '0' || r > '9' {
			return false
		}
	}
	return true
}

// fixChat replaces a placeholder chat name with a real one if one is known by now.
func (b *Bridge) fixChat(ctx context.Context, c Chat) Chat {
	if !isFallbackName(c.Name) {
		return c
	}
	jid, err := types.ParseJID(c.JID)
	if err != nil || jid.Server == types.GroupServer {
		return c // group names come from refreshGroupNames
	}
	name := b.contactName(ctx, jid)
	if !isFallbackName(name) {
		_ = b.store.EnsureChat(ctx, c.JID, name, false)
		c.Name = name
	} else if c.Name == "" || strings.HasPrefix(c.Name, "~") || !strings.HasPrefix(c.Name, "+") {
		c.Name = name
	}
	return c
}

// fixSenders replaces placeholder sender names in group messages.
func (b *Bridge) fixSenders(ctx context.Context, msgs []Msg) {
	cache := map[string]string{}
	for i := range msgs {
		m := &msgs[i]
		if m.FromMe || !isFallbackName(m.SenderName) || m.Sender == "" {
			continue
		}
		name, ok := cache[m.Sender]
		if !ok {
			name = m.SenderName
			if jid, err := types.ParseJID(m.Sender); err == nil {
				if n := b.contactName(ctx, jid); !isFallbackName(n) || name == "" {
					name = n
				}
			}
			cache[m.Sender] = name
			if !isFallbackName(name) {
				_ = b.store.SetSenderName(ctx, m.Sender, name)
			}
		}
		m.SenderName = name
	}
}

// chatName returns the stored name for a chat, resolving it from WhatsApp when missing.
func (b *Bridge) chatName(ctx context.Context, jid types.JID) string {
	if name := b.store.ChatName(ctx, jid.String()); !isFallbackName(name) {
		return name
	}
	if jid.Server == types.GroupServer {
		if info, err := b.wa.GetGroupInfo(ctx, jid); err == nil && info.Name != "" {
			return info.Name
		}
		return "Group"
	}
	return b.contactName(ctx, jid)
}

func (b *Bridge) renameChat(ctx context.Context, jid, name string) {
	if err := b.store.EnsureChat(ctx, jid, name, strings.HasSuffix(jid, "@"+types.GroupServer)); err != nil {
		log.Printf("rename chat %s: %v", jid, err)
		return
	}
	b.broadcastChat(ctx, jid)
}

func (b *Bridge) setUnread(ctx context.Context, jid string, n int) {
	if err := b.store.SetUnread(ctx, jid, n); err != nil {
		log.Printf("set unread %s: %v", jid, err)
		return
	}
	b.broadcastChat(ctx, jid)
}

func (b *Bridge) broadcastChat(ctx context.Context, jid string) {
	if c, err := b.store.GetChat(ctx, jid); err == nil && c.LastTS > 0 {
		c = b.fixChat(ctx, c)
		b.hub.Broadcast(line(append([]string{"*", "CHAT"}, c.fields()...)...))
	}
}

func (b *Bridge) refreshGroupNames() {
	ctx, cancel := context.WithTimeout(context.Background(), time.Minute)
	defer cancel()
	groups, err := b.wa.GetJoinedGroups(ctx)
	if err != nil {
		log.Printf("fetch groups: %v", err)
		return
	}
	for _, g := range groups {
		if g.Name != "" {
			_ = b.store.EnsureChat(ctx, g.JID.String(), g.Name, true)
		}
	}
}

// describe turns a WhatsApp message into a kind, display text and whether it has viewable media.
// ok is false for messages that have nothing to show (reactions, protocol messages, ...).
func describe(msg *waE2E.Message) (kind, text string, hasMedia, ok bool) {
	switch {
	case msg.GetConversation() != "":
		return "text", msg.GetConversation(), false, true
	case msg.GetExtendedTextMessage() != nil:
		return "text", msg.GetExtendedTextMessage().GetText(), false, true
	case msg.GetImageMessage() != nil:
		return "image", msg.GetImageMessage().GetCaption(), true, true
	case msg.GetStickerMessage() != nil:
		return "sticker", "", true, true
	case msg.GetVideoMessage() != nil:
		v := msg.GetVideoMessage()
		t := "[Video " + duration(v.GetSeconds()) + "]"
		if v.GetCaption() != "" {
			t += " " + v.GetCaption()
		}
		return "video", t, len(v.GetJPEGThumbnail()) > 0, true
	case msg.GetAudioMessage() != nil:
		a := msg.GetAudioMessage()
		// voice notes and audio files can be played on the 3DS (Ogg/Opus only)
		label := "Audio"
		if a.GetPTT() {
			label = "Voice message"
		}
		return "audio", "[" + label + " " + duration(a.GetSeconds()) + "]", true, true
	case msg.GetDocumentMessage() != nil:
		d := msg.GetDocumentMessage()
		name := d.GetFileName()
		if name == "" {
			name = d.GetTitle()
		}
		t := "[Document] " + name
		if d.GetCaption() != "" {
			t += "\n" + d.GetCaption()
		}
		return "document", t, false, true
	case msg.GetLocationMessage() != nil:
		l := msg.GetLocationMessage()
		t := fmt.Sprintf("[Location] %.5f, %.5f", l.GetDegreesLatitude(), l.GetDegreesLongitude())
		if l.GetName() != "" {
			t = "[Location] " + l.GetName()
		}
		return "other", t, false, true
	case msg.GetLiveLocationMessage() != nil:
		return "other", "[Live location]", false, true
	case msg.GetContactMessage() != nil:
		return "other", "[Contact] " + msg.GetContactMessage().GetDisplayName(), false, true
	case msg.GetContactsArrayMessage() != nil:
		return "other", "[Contacts] " + msg.GetContactsArrayMessage().GetDisplayName(), false, true
	case msg.GetPollCreationMessage() != nil:
		return "other", "[Poll] " + msg.GetPollCreationMessage().GetName(), false, true
	case msg.GetPollCreationMessageV2() != nil:
		return "other", "[Poll] " + msg.GetPollCreationMessageV2().GetName(), false, true
	case msg.GetPollCreationMessageV3() != nil:
		return "other", "[Poll] " + msg.GetPollCreationMessageV3().GetName(), false, true
	}
	return "", "", false, false
}

func duration(sec uint32) string {
	return fmt.Sprintf("%d:%02d", sec/60, sec%60)
}

func truncate(s string, n int) string {
	s = strings.ReplaceAll(s, "\n", " ")
	if utf8.RuneCountInString(s) <= n {
		return s
	}
	r := []rune(s)
	return string(r[:n-1]) + "…"
}

func previewOf(m Msg, isGroup bool) string {
	text := m.Text
	if text == "" {
		switch m.Kind {
		case "image":
			text = "[Photo]"
		case "sticker":
			text = "[Sticker]"
		}
	} else if m.Kind == "image" {
		text = "[Photo] " + text
	}
	switch {
	case m.FromMe:
		text = "You: " + text
	case isGroup && m.SenderName != "":
		text = m.SenderName + ": " + text
	}
	return truncate(text, 80)
}

// handleMessage stores a WhatsApp message and, when live, pushes it to connected clients.
func (b *Bridge) handleMessage(ctx context.Context, evt *events.Message, live bool) {
	chat := b.normalizeChat(ctx, evt.Info.Chat)
	if chat.Server == types.HiddenUserServer && !evt.Info.IsGroup {
		alt := evt.Info.SenderAlt
		if evt.Info.IsFromMe {
			alt = evt.Info.RecipientAlt
		}
		if alt.Server == types.DefaultUserServer {
			chat = alt.ToNonAD()
		}
	}
	if skipChat(chat) || evt.Message == nil {
		return
	}
	chatJID := chat.String()

	// Edits and deletions change an existing message instead of adding one.
	if pm := evt.Message.GetProtocolMessage(); pm != nil {
		targetID := pm.GetKey().GetID()
		switch pm.GetType() {
		case waE2E.ProtocolMessage_REVOKE:
			b.updateMessage(ctx, chatJID, targetID, "deleted", "This message was deleted", true, live)
		case waE2E.ProtocolMessage_MESSAGE_EDIT:
			if kind, text, _, ok := describe(pm.GetEditedMessage()); ok {
				b.updateMessage(ctx, chatJID, targetID, kind, text, false, live)
			}
		}
		return
	}
	if evt.IsEdit {
		return // already handled through the protocol message above in current whatsmeow versions
	}

	kind, text, hasMedia, ok := describe(evt.Message)
	if !ok {
		return
	}

	isGroup := chat.Server == types.GroupServer
	if err := b.store.EnsureChat(ctx, chatJID, "", isGroup); err != nil {
		log.Printf("ensure chat: %v", err)
		return
	}
	// Fill in the chat name, or upgrade a placeholder now that a real name may be known.
	if name := b.store.ChatName(ctx, chatJID); isFallbackName(name) {
		newName := b.chatName(ctx, chat)
		if isFallbackName(newName) && !isGroup && !evt.Info.IsFromMe && evt.Info.PushName != "" {
			newName = evt.Info.PushName
		}
		if name == "" || !isFallbackName(newName) {
			_ = b.store.EnsureChat(ctx, chatJID, newName, isGroup)
		}
	}

	senderName := evt.Info.PushName
	if !evt.Info.IsFromMe {
		if n := b.contactName(ctx, evt.Info.Sender.ToNonAD()); n != "" && !strings.HasPrefix(n, "+") || senderName == "" {
			senderName = n
		}
	}

	m := Msg{
		Chat:       chatJID,
		ID:         evt.Info.ID,
		TS:         evt.Info.Timestamp.Unix(),
		FromMe:     evt.Info.IsFromMe,
		Sender:     evt.Info.Sender.ToNonAD().String(),
		SenderName: senderName,
		Kind:       kind,
		Text:       text,
	}
	if hasMedia {
		if raw, err := proto.Marshal(evt.Message); err == nil {
			m.Media = raw
		}
	}

	if err := b.store.PutMessage(ctx, m, previewOf(m, isGroup), live && !m.FromMe); err != nil {
		log.Printf("store message: %v", err)
		return
	}
	if live {
		if m.FromMe {
			// Reading or replying on the phone means the chat has been seen.
			_ = b.store.SetUnread(ctx, chatJID, 0)
		}
		b.hub.Broadcast(line(append([]string{"*", "MSG"}, m.fields()...)...))
		b.broadcastChat(ctx, chatJID)
	}
}

func (b *Bridge) updateMessage(ctx context.Context, chat, id, kind, text string, dropMedia, live bool) {
	m, err := b.store.UpdateMessageText(ctx, chat, id, kind, text, dropMedia)
	if errors.Is(err, sql.ErrNoRows) {
		return
	} else if err != nil {
		log.Printf("update message: %v", err)
		return
	}
	if live {
		b.hub.Broadcast(line(append([]string{"*", "MSG"}, m.fields()...)...))
	}
}

func (b *Bridge) handleHistorySync(ctx context.Context, evt *events.HistorySync) {
	convs := evt.Data.GetConversations()
	count := 0
	for _, conv := range convs {
		jid, err := types.ParseJID(conv.GetID())
		if err != nil {
			continue
		}
		chat := b.normalizeChat(ctx, jid)
		if skipChat(chat) {
			continue
		}
		name := conv.GetName()
		if name == "" {
			name = conv.GetDisplayName()
		}
		isGroup := chat.Server == types.GroupServer
		if err := b.store.EnsureChat(ctx, chat.String(), name, isGroup); err != nil {
			log.Printf("history chat: %v", err)
			continue
		}
		for _, hm := range conv.GetMessages() {
			parsed, err := b.wa.ParseWebMessage(jid, hm.GetMessage())
			if err != nil {
				continue
			}
			b.handleMessage(ctx, parsed, false)
			count++
		}
		if conv.UnreadCount != nil {
			_ = b.store.SetUnread(ctx, chat.String(), int(conv.GetUnreadCount()))
		}
	}
	log.Printf("History sync: %d conversations, %d messages", len(convs), count)
	b.cleanupChats(false)
	b.hub.Broadcast(line("*", "RELOAD"))
}

// SendText sends a text message and records it like an incoming event.
func (b *Bridge) SendText(ctx context.Context, chatJID, text string) (string, error) {
	jid, err := types.ParseJID(chatJID)
	if err != nil {
		return "", fmt.Errorf("bad jid: %w", err)
	}
	msg := &waE2E.Message{Conversation: proto.String(text)}
	resp, err := b.wa.SendMessage(ctx, jid, msg)
	if err != nil {
		return "", err
	}
	b.recordSent(ctx, jid, resp, msg)
	return resp.ID, nil
}

// MarkChatRead sends read receipts for the latest incoming messages and clears the unread counter.
func (b *Bridge) MarkChatRead(ctx context.Context, chatJID string) error {
	chat, err := types.ParseJID(chatJID)
	if err != nil {
		return err
	}
	bySender, err := b.store.UnreadIncoming(ctx, chatJID, 20)
	if err != nil {
		return err
	}
	for senderStr, ids := range bySender {
		sender, _ := types.ParseJID(senderStr)
		if chat.Server != types.GroupServer {
			sender = types.EmptyJID
		}
		if err := b.wa.MarkRead(ctx, ids, time.Now(), chat, sender); err != nil {
			log.Printf("mark read %s: %v", chatJID, err)
		}
	}
	b.setUnread(ctx, chatJID, 0)
	return nil
}

func (b *Bridge) OwnInfo() (name, jid string) {
	if b.wa.Store.ID != nil {
		jid = b.wa.Store.ID.ToNonAD().String()
	}
	return b.wa.Store.PushName, jid
}

func isCallEvent(evt any) bool {
	switch evt.(type) {
	case *events.CallOffer, *events.CallAccept, *events.CallTransport, *events.CallTerminate, *events.CallReject:
		return true
	}
	return false
}

// renameIfKnown updates an existing chat's name. Push names only replace placeholders,
// never a name from the address book.
func (b *Bridge) renameIfKnown(ctx context.Context, jid, name string, onlyPlaceholder bool) {
	cur, err := b.store.GetChat(ctx, jid)
	if err != nil || cur.IsGroup || cur.Name == name {
		return
	}
	if onlyPlaceholder && !isFallbackName(cur.Name) {
		return
	}
	b.renameChat(ctx, jid, name)
}

// cleanupChats merges chats stored under a LID into the phone-number chat for the same person
// and upgrades placeholder names. With notify, clients are told to refetch if anything changed.
func (b *Bridge) cleanupChats(notify bool) {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()
	lids, err := b.store.ChatJIDsOnServer(ctx, types.HiddenUserServer)
	if err != nil {
		log.Printf("cleanup chats: %v", err)
		return
	}
	merged := 0
	for _, l := range lids {
		lid, err := types.ParseJID(l)
		if err != nil {
			continue
		}
		pn, err := b.wa.Store.LIDs.GetPNForLID(ctx, lid)
		if err != nil || pn.IsEmpty() {
			continue
		}
		if err := b.store.MergeChat(ctx, l, pn.ToNonAD().String()); err != nil {
			log.Printf("merge chat %s: %v", l, err)
			continue
		}
		merged++
	}
	renamed := 0
	chats, err := b.store.Chats(ctx, 1000)
	if err == nil {
		for _, c := range chats {
			if isFallbackName(c.Name) && !isFallbackName(b.fixChat(ctx, c).Name) {
				renamed++
			}
		}
	}
	if merged > 0 || renamed > 0 {
		log.Printf("Chat cleanup: merged %d duplicate LID chats, found names for %d chats", merged, renamed)
		if notify {
			b.hub.Broadcast(line("*", "RELOAD"))
		}
	}
}
