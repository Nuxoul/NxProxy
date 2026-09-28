package rpc

import (
	"context"

	"ThroneCore/gen"

	"github.com/sagernet/sing-box/adapter"
	"github.com/sagernet/sing-box/protocol/group"
	"github.com/sagernet/sing/service"
)

// Runtime switch for a manual (Clash-style) strategy group.
//
// The generated config bakes one member in as the group's default, but a live selector
// accepts a new pick, so a click in Throne never has to reload the whole config.
// Throne still persists the choice, so the next config keeps it.
func (s *server) SelectOutbound(_ context.Context, in *gen.SelectOutboundRequest) (*gen.ErrorResp, error) {
	groupTag := in.GetGroupTag()
	if groupTag == "" {
		return &gen.ErrorResp{Error: To("group tag is required")}, nil
	}
	box := currentBox()
	if box == nil {
		return &gen.ErrorResp{Error: To(errInstanceNotRunning.Error())}, nil
	}
	outbounds := service.FromContext[adapter.OutboundManager](box.Context())
	if outbounds == nil {
		return &gen.ErrorResp{Error: To(errInstanceNotRunning.Error())}, nil
	}
	outbound, loaded := outbounds.Outbound(groupTag)
	if !loaded {
		return &gen.ErrorResp{Error: To("no outbound with tag " + groupTag + " in the running config")}, nil
	}
	selector, isSelector := outbound.(*group.Selector)
	if !isSelector {
		return &gen.ErrorResp{Error: To("outbound " + groupTag + " is not a strategy group")}, nil
	}
	memberTag := in.GetOutboundTag()
	if !selector.SelectOutbound(memberTag) {
		return &gen.ErrorResp{Error: To("group " + groupTag + " does not list " + memberTag)}, nil
	}
	return &gen.ErrorResp{Error: To("")}, nil
}
