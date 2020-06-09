#include "envoy/extensions/filters/network/echo/v3/rot13.pb.h"
#include "envoy/extensions/filters/network/echo/v3/rot13.pb.validate.h"
#include "envoy/registry/registry.h"
#include "envoy/server/filter_config.h"

#include "extensions/filters/network/common/factory_base.h"
#include "extensions/filters/network/echo/echo.h"
#include "extensions/filters/network/well_known_names.h"

namespace Envoy {
namespace Extensions {
namespace NetworkFilters {
namespace Rot13 {

/**
 * Config registration for the echo filter. @see NamedNetworkFilterConfigFactory.
 */
class Rot13ConfigFactory
    : public Common::FactoryBase<envoy::extensions::filters::network::echo::v3::Rot13> {
public:
  Rot13ConfigFactory() : FactoryBase(NetworkFilterNames::get().Rot13) {}

private:
  Network::FilterFactoryCb
  createFilterFactoryFromProtoTyped(const envoy::extensions::filters::network::echo::v3::Rot13&,
                                    Server::Configuration::FactoryContext&) override {
    return [](Network::FilterManager& filter_manager) -> void {
      filter_manager.addReadFilter(std::make_shared<Rot13Filter>());
    };
  }

  bool isTerminalFilter() override { return true; }
};

/**
 * Static registration for the echo filter. @see RegisterFactory.
 */
REGISTER_FACTORY(Rot13ConfigFactory,
                 Server::Configuration::NamedNetworkFilterConfigFactory){"envoy.echo"};

} // namespace Rot13
} // namespace NetworkFilters
} // namespace Extensions
} // namespace Envoy
